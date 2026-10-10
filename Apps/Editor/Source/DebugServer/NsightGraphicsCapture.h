#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace GameEngine
{

// Thin wrapper around the NVIDIA Nsight Graphics in-application capture API.
// Only activates when the editor was launched by ngfx-capture.exe, which
// injects the capture libraries into the process. Never loads them itself.
class NsightGraphicsCapture
{
  public:
    NsightGraphicsCapture() = default;

    // Bind to the already-injected Graphics Capture activity.
    // Safe to call when Nsight is absent -- returns false.
    bool Initialize();

    bool IsAvailable() const { return m_Initialized; }

    // Request a capture beginning at the next present.
    // frameCount is clamped to the SDK's supported [1, 60] range.
    //
    // Returns nullopt when the SDK accepted the request, or the reason it was
    // rejected. A rejected request produces no capture file, so callers that
    // wait for one must report the failure instead of waiting.
    [[nodiscard]] std::optional<std::string> TriggerCapture(uint32_t frameCount);

    // Return how many capture files have completed this session.
    uint32_t GetCaptureCount() const;

    // Return the path of the most recently completed capture, or empty if none.
    std::string GetLastCapturePath() const;

  private:
    // The NGFX SDK is header-only and owns a process-wide function table, so
    // there is no API object to hold -- only whether this process bound to it.
    // Keeping the NGFX headers in the .cpp also keeps <Windows.h> out of every
    // translation unit that wants to trigger a capture.
    bool m_Initialized = false;
};

} // namespace GameEngine

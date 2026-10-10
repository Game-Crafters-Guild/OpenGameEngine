#pragma once

#include <cstdint>
#include <string>

namespace GameEngine
{

// Thin wrapper around the RenderDoc in-application API.
// Only activates when the editor is running under RenderDoc (the DLL is
// already injected). Does not load RenderDoc on its own.
class RenderDocCapture
{
  public:
    RenderDocCapture() = default;

    // Attempt to connect to an already-injected RenderDoc instance.
    // Safe to call even when RenderDoc is not present -- returns false.
    bool Initialize();

    bool IsAvailable() const { return m_Api != nullptr; }

    // Schedule the next presented frame for capture.
    void TriggerCapture();

    // Return the file path of the most recent capture, or empty if none.
    std::string GetLastCapturePath() const;

    // Return how many captures have been made this session.
    uint32_t GetCaptureCount() const;

  private:
    // Point RenderDoc at a predictable directory: GE_RENDERDOC_CAPTURE_DIR when
    // set, else <exeDir>/Captures. A template a launcher chose deliberately is
    // left alone; only RenderDoc's own %TEMP%/RenderDoc default is replaced.
    void ApplyCaptureFilePathTemplate();

    void* m_Api = nullptr; // RENDERDOC_API_1_6_0*, cast in .cpp to avoid header dependency
};

} // namespace GameEngine

#pragma once

#include "Engine/Rendering/ViewReadbackUtils.h"
#include "UI/UITextureSpace.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace GameEngine
{
namespace Rendering
{
namespace RenderGraph
{
class RGFrame;
}
}
namespace Engine::Renderer
{
class RenderServices;
}

namespace Editor
{

// Owns the scene-save thumbnail capture pipeline: schedules a main-view readback
// after each successful scene save, converts the pixels to sRGB8 and writes a PNG
// on a background thread, then signals the main thread to refresh the asset cell.
class SceneThumbnailCapture
{
public:
    // Signaled on the main thread once the background PNG write has finished.
    // cachePath: absolute path of the written PNG (for UI texture eviction).
    // scenePath: the .scene file path (for asset cell lookup).
    using ReadyCallback = std::function<void(const std::string& cachePath,
                                              const std::string& scenePath)>;

    // Schedule a capture: called from the scene save callback. The actual readback
    // is started on the next RecordFrame() that finds a scene view.
    void RequestCapture(const std::filesystem::path& scenePath);

    // Called from RecordWindowWorldViews; returns true if a capture should be
    // recorded into the render graph this frame.
    bool WantsCapture() const { return m_Pending && !m_Ticket && !m_PendingPath.empty(); }

    // RenderGraph arm (8c-4): ticket readback of the scene view's pipeline output —
    // the texture the UI actually samples (post-FX FinalColor), so what you
    // see is what you save. Call post-spine on the frame the view declared.
    void BeginReadbackRG(Rendering::RenderGraph::RGFrame& frame, uint32_t sceneViewId,
                         Engine::Renderer::RenderServices* rs);

    // Polls the in-flight readback + the ready flag. Invokes onReady on the main
    // thread (synchronously) when a PNG write has completed.
    void Poll(const ReadyCallback& onReady);

private:
    void CommitResult(const Rendering::ViewReadbackResult& result, UI::UITextureSpace sourceSpace);

    // Result of the background PNG write. Held on the heap behind a shared_ptr so
    // the detached writer thread captures a copy and signals through it without
    // dereferencing `this` — the capture can be destroyed while the write runs.
    struct Ready
    {
        std::mutex Mutex;
        std::string CachePath;
        std::string ScenePath;
        bool Flag = false;
    };

    bool m_Pending = false;
    std::filesystem::path m_PendingPath;
    std::filesystem::path m_InFlightPath;
    std::shared_ptr<Rendering::RGReadbackTicket> m_Ticket;    // RenderGraph arm (8c-1)
    // The producer's stamp for the pixels this ticket is copying, taken at
    // declaration and carried to the PNG write. Engaged exactly while m_Ticket is:
    // no default value exists, so a readback can never be converted under a space
    // nobody stated.
    std::optional<UI::UITextureSpace> m_InFlightSpace;

    std::shared_ptr<Ready> m_Ready = std::make_shared<Ready>();
};

} // namespace Editor
} // namespace GameEngine

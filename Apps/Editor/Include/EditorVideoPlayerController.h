#pragma once

#include <filesystem>

namespace GameEngine {

namespace Rendering { class IDevice; }
namespace Rendering::RenderGraph { class RGFrame; }
class VideoPlayerModal;
class UIElement;
class UIManager;

// Manages the VideoPlayerModal lifecycle — creation, mounting, playback, and per-frame update.
// EditorApplication creates one instance; toolbar buttons call Open() directly.
class EditorVideoPlayerController
{
public:
    // Creates the VideoPlayerModal and mounts it as a child of root. Call once during UI setup.
    void Setup(UIElement* root, Rendering::IDevice* device, UIManager* uiManager);

    // Opens the modal and begins playback of videoPath.
    // If title is non-empty it overrides the filename-derived header text.
    void Open(const std::filesystem::path& videoPath, const std::string& title = {});

    bool IsOpen() const;
    void Close();

    // Stage whatever frame the player has decoded, if any. Call every frame,
    // before the render-graph frame is declared. The player paces its own
    // decoding, so this never decodes and takes no tick duration.
    void Update();

    // Declare the staged-frame upload inside the host's render-graph frame and
    // publish the texture for the UI to sample. Call once per render-graph
    // frame, during declaration, after Update.
    void TickRenderRG(Rendering::RenderGraph::RGFrame& frame);

private:
    VideoPlayerModal* m_Modal = nullptr; // non-owning; UI tree owns the element
};

} // namespace GameEngine

#pragma once

#include "Core/Application.h"

#include <memory>
#include <string>

namespace GameEngine
{

class RuntimeHost;

namespace Engine::Renderer { class RenderServices; }

namespace WebLibrary
{

/// The engine as a page drives it: one RuntimeHost on the page's canvas, an empty primary
/// world, and one frame per Tick. The page owns the frame cadence, the camera and everything
/// in the world; this class owns the window, the device and the render pipeline.
class WebLibraryApplication final : public Application
{
public:
    explicit WebLibraryApplication(const ApplicationConfig& config);
    ~WebLibraryApplication() override;

    /// Brings up the engine, the window on the page's canvas, the WebGPU device (suspends
    /// while the browser answers) and the default render pipeline, and creates the empty
    /// primary world. False, with the reason in `outError`, when any step fails.
    bool Start(std::string& outError);

    /// Sizes the window to the canvas's CSS size; the drawing buffer follows at the device
    /// pixel ratio.
    void Resize(int width, int height);

    /// Null before Start succeeds and after Shutdown.
    Engine::Renderer::RenderServices* GetRenderServices() const;

protected:
    void Update(float64 deltaTime) override;
    void Render() override;
    void OnShutdown() override;

private:
    std::unique_ptr<RuntimeHost> m_Host;
    float m_LastRenderDeltaTime = 1.0f / 60.0f;
};

} // namespace WebLibrary
} // namespace GameEngine

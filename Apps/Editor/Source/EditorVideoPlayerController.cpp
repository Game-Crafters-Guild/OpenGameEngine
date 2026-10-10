#include "EditorVideoPlayerController.h"
#include "Panels/VideoPlayerModal.h"
#include "UI/UIElement.h"
#include "Rendering/Core/Device.h"
#include "UI/UIManager.h"

#include <memory>

namespace GameEngine {

void EditorVideoPlayerController::Setup(UIElement* root, Rendering::IDevice* device, UIManager* uiManager)
{
    if (!root) return;
    auto modal = std::make_unique<VideoPlayerModal>();
    modal->SetDevice(device);
    modal->SetUIManager(uiManager);
    m_Modal = modal.get();
    root->AddChild(std::move(modal));
}

void EditorVideoPlayerController::Open(const std::filesystem::path& videoPath, const std::string& title)
{
    if (m_Modal)
        m_Modal->Open(videoPath.string(), title);
}

bool EditorVideoPlayerController::IsOpen() const
{
    return m_Modal && m_Modal->IsOpen();
}

void EditorVideoPlayerController::Close()
{
    if (m_Modal)
        m_Modal->Close();
}

void EditorVideoPlayerController::Update()
{
    if (m_Modal)
        m_Modal->Update();
}

void EditorVideoPlayerController::TickRenderRG(Rendering::RenderGraph::RGFrame& frame)
{
    if (m_Modal && m_Modal->IsOpen())
        m_Modal->TickRenderRG(frame);
}

} // namespace GameEngine

#pragma once

#include "Engine/Rendering/ViewReadbackUtils.h"
#include "SceneViewController.h"
#include "UI/UIElement.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class IDevice;
namespace RenderGraph
{
class RGFrame;
}
}
class Button;
class INativeContextMenu;
class SceneViewPanel;

class CameraBookmarksWidget : public UIElement
{
public:
    CameraBookmarksWidget();
    ~CameraBookmarksWidget() override;

    void SetSceneController(SceneViewController* controller)
    {
        if (m_Controller != controller)
        {
            if (m_Controller)
                m_Controller->CancelBookmarkPreview();
            m_Controller = controller;
            m_ExternalBound = false;
            m_PendingPreviewSerial = 0;
            m_WaitingForFreshPreview = false;
            m_WaitingPreviewFrames = 0;
        }
    }
    void SetPanel(SceneViewPanel* panel) { m_Panel = panel; }

    void OnPostLayout() override;
    void OnOwnerManagerChanged(UIManager* owner) override;
    
    // Update readback requests (call this periodically to poll for captured screenshots)
    void Update();
    
    // Process pending screenshot requests — called by the controller's
    // post-spine FinalizePreviewRG on frames where the preview rendered.
    // Tickets resolve in Update() once stamped + GPU-signaled (8c-1).
    bool ProcessPendingScreenshotRequestsRG(GameEngine::Rendering::RenderGraph::RGFrame& frame);

    /** True while the hover preview popup is visible (preview texture should keep updating). */
    bool IsPreviewPopupOpen() const;

  private:
    struct Bookmark
    {
        SceneViewCameraPose pose;
        std::vector<uint8_t> previewPixels;  // RGBA8, 128x72
    };

    void RebuildButtons();
    void AddBookmark();
    void ReplaceBookmark(size_t index);
    void RecallBookmark(size_t index);
    void DeleteBookmark(size_t index);

    /// Right-click menu for one slot, at window-space (x, y). `index` is the
    /// numbered slot; kDefaultSlotIndex is the always-present default view,
    /// which resets instead of deleting because it cannot be removed.
    void ShowSlotContextMenu(int index, float windowX, float windowY);
    static constexpr int kDefaultSlotIndex = -1;
    
    // Default bookmark (icon button, always visible)
    void SaveDefaultBookmark();
    void RecallDefaultBookmark();
    void ResetDefaultBookmark();
    void CaptureDefaultPreview(Button* b);
    /// Binds the controller's frozen preview snapshot into the popup's UI slot,
    /// carrying the controller's stored space stamp (#767) so the widget never
    /// derives a space from the display. When no readable snapshot exists —
    /// unwritten, or the device rebuilt since it was written — the slot is
    /// evicted rather than left bound, because the UI holds its own reference
    /// and nothing else would drop a handle the rebuild already freed.
    void BindPreviewSnapshot();

    std::unique_ptr<INativeContextMenu> m_ContextMenu;

    SceneViewController* m_Controller = nullptr;
    SceneViewPanel* m_Panel = nullptr;
    UIElement* m_Row = nullptr;

    const uint32 MAX_BOOKMARKS = 9;
    std::vector<Bookmark> m_Bookmarks;
    
    // Default bookmark (always visible icon button)
    Bookmark m_DefaultBookmark;
    bool m_DefaultBookmarkSet = false;
    bool m_ExternalBound = false;

    // Preview render target dimensions (must match SceneViewController render targets).
    static constexpr uint32 kPreviewWidth = 128;
    static constexpr uint32 kPreviewHeight = 72;
    // Frames to wait for the snapshot bind after hover before giving up. The
    // preview view itself retries draw-empty for 16 frames; this is looser so
    // a late copy still binds.
    static constexpr int kPreviewSnapshotWaitFrames = 32;

    std::vector<bool> m_IsPreviewOpen;
    // Screenshot readback tickets (8c-1): null = no readback in flight.
    std::vector<std::shared_ptr<Rendering::RGReadbackTicket>> m_PendingReadbacks;
    std::vector<size_t> m_PendingScreenshotRequests; // Bookmarks that need screenshots captured
    void CaptureScenePreview(size_t index, Button* b);
    /// Places the hover preview under `slot`, clamped so it stays on-window.
    void PositionPreviewPopup(UIElement* popup, const Button& slot);
    void RemovePreview(size_t index);
    void UpdateReadbacks(); // Poll readback requests and store pixels
    void RequestScreenshotForBookmark(size_t index); // Request screenshot capture for a bookmark
    void DisplayStoredPreview(size_t index);
    
    // Save/Load bookmarks using Settings API
    void SaveBookmarks();
    void LoadBookmarks();
    void SavePreviewImage(size_t index, uint32_t width, uint32_t height,
                          Rendering::TextureFormat format = Rendering::TextureFormat::RGBA8_UNORM);
    std::filesystem::path GetBookmarkPreviewPath(size_t index) const;

    void ClosePreview();

    // Fallback close: if MouseLeave is missed, close the preview as soon as the mouse
    // target is no longer within the active bookmark button (wired at the UI root).
    void EnsureRootMouseMoveFallback();
    void RemoveRootMouseMoveFallback();
    UIElement* m_RootMouseMoveElement = nullptr; // not owned
    EventHandlerToken m_RootMouseMoveToken{};

    int m_ActivePreviewIndex = -1;
    std::string m_ActivePreviewButtonId;
    Button* ResolveActivePreviewButton();

    std::uint64_t m_PendingPreviewSerial = 0;
    bool m_WaitingForFreshPreview = false;
    int m_WaitingPreviewFrames = 0;

    int m_ActiveBookmarkIndex = -1; // Currently recalled/active bookmark
    void UpdateActiveBookmarkVisual();
};
} // namespace GameEngine

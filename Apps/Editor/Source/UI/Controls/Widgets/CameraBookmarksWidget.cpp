#include "UI/Controls/Widgets/CameraBookmarksWidget.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewReadbackUtils.h"
#include "Rendering/Core/Device.h"
#include "Input/KeyCodes.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Panels/SceneViewPanel.h"
#include "Platform/ContextMenu.h"
#include "SceneViewController.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/SceneViewToolbar.h"
#include "UI/EditorIcons.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/StyleProperties.h"
#include "Core/Engine.h"
#include "Core/Application.h"
#include "Editor/EditorPaths.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector2.h"
#include "UI/Registration/ElementRegistration.h"

#include <cmath>
#include <memory>
#include <stb_image_write.h>
#include <stb_image.h>

#include <filesystem>
#include <algorithm>
#include <fstream>

using namespace GameEngine;
using namespace GameEngine::UI;

namespace
{
// The UI texture key the popup's CSS resolves via engine("...").
constexpr const char* kPreviewTextureSlot = "editor_camera_bookmark_preview";

constexpr uint32_t kCmdBookmarkDelete = 0x5B01;
constexpr uint32_t kCmdBookmarkReset  = 0x5B02;

bool IsPointInside(const UIElement& el, float x, float y)
{
    const float lx = el.GetLayoutX();
    const float ly = el.GetLayoutY();
    const float w = el.GetLayoutWidth();
    const float h = el.GetLayoutHeight();
    return (x >= lx && y >= ly && x < (lx + w) && y < (ly + h));
}
} // namespace

CameraBookmarksWidget::CameraBookmarksWidget()
{
    AddClass("camera-bookmarks");

    // row div for all buttons (1 2 3 ... +)
    auto row = std::make_unique<UIElement>();
    row->AddClass("camera-bookmarks-row");

    m_Row = row.get();
    UIElement::AddChild(std::move(row));

    // Default pose matches SceneViewController / seeded Main Camera (0,3,-10, +Z forward).
    m_DefaultBookmark.pose.Pos[0] = 0.0f;
    m_DefaultBookmark.pose.Pos[1] = 3.0f;
    m_DefaultBookmark.pose.Pos[2] = -10.0f;
    m_DefaultBookmark.pose.YawDeg = 90.0f;
    m_DefaultBookmark.pose.PitchDeg = 0.0f;
    m_DefaultBookmark.pose.Distance = 6.0f;
    m_DefaultBookmarkSet = true;

    // Load saved bookmarks (will override default if saved)
    LoadBookmarks();
    
    // Default bookmark is always active on startup (set before RebuildButtons)
    m_ActiveBookmarkIndex = -2;
    
    // Build initial buttons
    RebuildButtons();
}

void CameraBookmarksWidget::OnPostLayout()
{
    // No-op: stylesheet is loaded by parent panel
}

void CameraBookmarksWidget::OnOwnerManagerChanged(UIManager* owner)
{
    // Any ownership move (including detach) invalidates root-level fallback state and
    // external texture registration scope.
    m_RootMouseMoveToken = {};
    m_RootMouseMoveElement = nullptr;
    m_ExternalBound = false;

    if (!owner)
    {
        // Detached from a UI tree: ensure preview state cannot remain logically "open".
        m_WaitingForFreshPreview = false;
        m_WaitingPreviewFrames = 0;
        m_PendingPreviewSerial = 0;
        m_ActivePreviewIndex = -1;
        m_ActivePreviewButtonId.clear();
        if (m_Controller)
            m_Controller->CancelBookmarkPreview();
    }
    else
    {
        UpdateActiveBookmarkVisual();
    }
}

CameraBookmarksWidget::~CameraBookmarksWidget()
{
    // Ensure we don't leave a dangling root-level handler capturing `this`.
    RemoveRootMouseMoveFallback();
    // IMPORTANT:
    // Do not call ClosePreview() here. During shutdown the SceneViewController (or its
    // RenderServices dependency) may already be destroyed, and ClosePreview() can call
    // back into that stack via CancelBookmarkPreview(), causing a use-after-free.
}

void CameraBookmarksWidget::RebuildButtons()
{
    if (!m_Row)
        return;

    // Ensure vectors are in sync before rebuilding
    if (m_IsPreviewOpen.size() != m_Bookmarks.size())
        m_IsPreviewOpen.assign(m_Bookmarks.size(), false);
    if (m_PendingReadbacks.size() != m_Bookmarks.size())
        m_PendingReadbacks.resize(m_Bookmarks.size());

    auto rebuild = [this]()
    {
        // Only close preview if there's something to close
        if (m_ActivePreviewIndex != -1 || !m_ActivePreviewButtonId.empty())
        {
            ClosePreview();
        }

        // Clear children. BOUNDED on purpose: RemoveChild defers (does not
        // shrink the list) when called during event dispatch, so a
        // while-not-empty loop here spins forever if a dispatch guard ever
        // regresses (the f16db2664 hang). One pointer-snapshot pass removes
        // each child exactly once either way.
        if (m_Row)
        {
            std::vector<UIElement*> toRemove;
            toRemove.reserve(m_Row->GetChildren().size());
            for (const auto& child : m_Row->GetChildren())
                if (child)
                    toRemove.push_back(child.get());
            for (UIElement* child : toRemove)
                m_Row->RemoveChild(child);
        }

        // Default camera bookmark (icon, always visible at index 0)
        {
            auto camIcon = std::make_unique<Button>();
            camIcon->SetId("CameraBookmark_Default");
            camIcon->AddClass("camera-bookmarks-slot");
            camIcon->AddClass("cambookmark-icon");
            Button* camPtr = camIcon.get();
            
            // Same behavior as numbered bookmarks: click to recall, Ctrl/Cmd+click to replace, right-click to reset
            camPtr->RegisterEventHandler(kEventMouseUp, [this, camPtr](UIEvent& e)
                                         {
                if (!IsPointInside(*camPtr, e.X, e.Y))
                    return;

                if (e.Button == Input::kMouseButton_Right)
                {
                    ShowSlotContextMenu(kDefaultSlotIndex, e.X, e.Y);
                    e.Stop();
                    return;
                }

                if (e.Button == Input::kMouseButton_Left)
                {
                    if (Input::IsPrimaryShortcutModifier(e.Mods))
                        SaveDefaultBookmark();
                    else
                        RecallDefaultBookmark();
                    e.Stop();
                    return;
                } });

            camPtr->RegisterEventHandler(kEventMouseEnter, [this, camPtr](UIEvent&) { CaptureDefaultPreview(camPtr); });
            // Let root fallback decide closure so transitions between bookmark and popup
            // do not thrash preview open/close state.
            camPtr->RegisterEventHandler(kEventMouseLeave, [this](UIEvent&) { EnsureRootMouseMoveFallback(); });

            m_Row->AddChild(std::move(camIcon));
        }

        // number buttons
        for (size_t i = 0; i < m_Bookmarks.size(); ++i)
        {
            auto b = std::make_unique<Button>();
            b->SetId("CameraBookmark_" + std::to_string(i + 1));
            b->AddClass("camera-bookmarks-slot");
            b->SetText(std::to_string(i + 1));
            Button* bPtr = b.get();

            // Use element-level events so we can observe modifiers (Ctrl/Cmd) and
            // handle right-click without bloating the core Button API.
            bPtr->RegisterEventHandler(kEventMouseUp, [this, i, bPtr](UIEvent& e)
                                       {
                if (!IsPointInside(*bPtr, e.X, e.Y))
                    return;

                if (e.Button == Input::kMouseButton_Right)
                {
                    ShowSlotContextMenu(static_cast<int>(i), e.X, e.Y);
                    e.Stop();
                    return;
                }

                if (e.Button == Input::kMouseButton_Left)
                {
                    if (Input::IsPrimaryShortcutModifier(e.Mods))
                        ReplaceBookmark(i);
                    else
                        RecallBookmark(i);
                    e.Stop();
                    return;
                } });

            bPtr->RegisterEventHandler(kEventKeyDown, [this, i](UIEvent& e)
                                       {
                if (e.Key == Input::kKeyCode_Space || e.Key == Input::kKeyCode_Enter)
                {
                    RecallBookmark(i);
                    e.Stop();
                } });

            // Use element-level hover events for preview popups
            bPtr->RegisterEventHandler(kEventMouseEnter, [this, i, bPtr](UIEvent&) { CaptureScenePreview(i, bPtr); });
            // Let root fallback decide closure so transitions between bookmark and popup
            // do not thrash preview open/close state.
            bPtr->RegisterEventHandler(kEventMouseLeave, [this, i](UIEvent&) {
                (void)i;
                EnsureRootMouseMoveFallback();
            });

            m_Row->AddChild(std::move(b));
        }

        // trailing "+" button to add new bookmarks
        if (m_Bookmarks.size() < MAX_BOOKMARKS)
        {
            auto add = std::make_unique<Button>();
            add->AddClass("icon-button");
            add->AddClass("plus-icon");
            add->SetTooltip("Add Camera Bookmark");
            Button* addPtr = add.get();
            addPtr->RegisterEventHandler(kEventMouseUp, [this, addPtr](UIEvent& e)
                                         {
                if (e.Button == Input::kMouseButton_Left && IsPointInside(*addPtr, e.X, e.Y))
                {
                    AddBookmark();
                    e.Stop();
                } });
            addPtr->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
                                         {
                if (e.Key == Input::kKeyCode_Space || e.Key == Input::kKeyCode_Enter)
                {
                    AddBookmark();
                    e.Stop();
                } });
            m_Row->AddChild(std::move(add));
        }
    };

    if (IsInEventDispatch())
    {
        PostAction([this, rebuild]() {
            rebuild();
            // Update visuals after rebuild completes
            if (m_Row && m_Row->GetOwnerManager() != nullptr)
            {
                UpdateActiveBookmarkVisual();
            }
        });
    }
    else
    {
        rebuild();
        // Update visuals after rebuild completes
        if (m_Row && m_Row->GetOwnerManager() != nullptr)
        {
            UpdateActiveBookmarkVisual();
        }
    }
}

void CameraBookmarksWidget::AddBookmark()
{
    if (!m_Controller)
        return;

    Bookmark bm{};
    bm.pose = m_Controller->GetCameraPose();
    size_t index = m_Bookmarks.size();
    m_Bookmarks.push_back(bm);
    m_IsPreviewOpen.push_back(false);
    
    // Request screenshot capture for this new bookmark
    RequestScreenshotForBookmark(index);
    
    // Make the new bookmark the active one
    m_ActiveBookmarkIndex = static_cast<int>(index);
    
    // Save bookmarks immediately
    SaveBookmarks();

    RebuildButtons();
}

void CameraBookmarksWidget::ReplaceBookmark(size_t index)
{
    if (!m_Controller || index >= m_Bookmarks.size())
        return;

    auto doReplace = [this, index]()
    {
        // Re-check validity since this may run deferred
        if (!m_Controller || index >= m_Bookmarks.size())
            return;
            
        m_Bookmarks[index].pose = m_Controller->GetCameraPose();
        // Clear old preview pixels - will be recaptured
        m_Bookmarks[index].previewPixels.clear();

        if (index < m_IsPreviewOpen.size())
            m_IsPreviewOpen[index] = false;

        // Request screenshot capture for this bookmark
        RequestScreenshotForBookmark(index);
        
        // Save bookmarks immediately
        SaveBookmarks();

        RebuildButtons();
    };

    if (IsInEventDispatch())
        PostAction(doReplace);
    else
        doReplace();
}

void CameraBookmarksWidget::RecallBookmark(size_t index)
{
    if (!m_Controller || !m_Panel || index >= m_Bookmarks.size())
        return;

    const auto& pose = m_Bookmarks[index].pose;

    // Don't switch projection immediately — let the tween finish and apply
    // the target pose (including Is2D) so the transition is visually smooth.
    m_Controller->StartCameraTween(pose, 0.7f);

    // update UI Panel camera for when the tween finishes
    m_Panel->SetYawPitch(pose.YawDeg, pose.PitchDeg);

    // Update the 2D/3D toolbar icon to reflect the bookmark's mode.
    if (auto* toolbar = dynamic_cast<SceneViewToolbar*>(GetParent()))
        toolbar->UpdateView2DButtonState();

    // Mark this bookmark as active
    m_ActiveBookmarkIndex = static_cast<int>(index);
    UpdateActiveBookmarkVisual();
}

void CameraBookmarksWidget::UpdateActiveBookmarkVisual()
{
    if (!m_Row)
        return;
    
    // Ensure m_Row is still valid and has an owner manager
    if (m_Row->GetOwnerManager() == nullptr)
        return;
    
    try
    {
        int numberedIndex = 0; // Index for numbered buttons (0, 1, 2, ...)
        for (auto& child : m_Row->GetChildren())
        {
            if (!child)
                continue;
            
            // Ensure child is still valid
            if (child->GetOwnerManager() == nullptr)
                continue;
                
            // Skip the "+" button (it has plus-icon class)
            if (child->HasClass("plus-icon"))
                continue;
            
            bool isActive = false;
            if (child->HasClass("cambookmark-icon"))
            {
                // Default bookmark is active when m_ActiveBookmarkIndex == -2
                isActive = (m_ActiveBookmarkIndex == -2);
            }
            else
            {
                // Numbered buttons: check if this index matches
                isActive = (numberedIndex == m_ActiveBookmarkIndex);
                ++numberedIndex;
            }
            
            if (isActive)
                child->AddClass("active");
            else
                child->RemoveClass("active");
        }
    }
    catch (...)
    {
        // If updating visuals fails, just return - don't crash
    }
}

void CameraBookmarksWidget::PositionPreviewPopup(UIElement* popup, const Button& slot)
{
    // Mirrors .camera-bookmarks-preview in SceneViewPanel.css.
    constexpr float kPopupWidth = 128.0f;
    constexpr float kPopupHeight = 72.0f;
    constexpr float kSlotGap = 10.0f;
    constexpr float kWindowMargin = 8.0f;

    // The popup is positioned relative to its parent, not the slot.
    const UIElement* anchor = popup->GetParent();
    const float ax = anchor ? anchor->GetLayoutX() : 0.0f;
    const float ay = anchor ? anchor->GetLayoutY() : 0.0f;

    // Horizontally the popup follows its slot; vertically it hangs off the row,
    // not the slot. The default slot is 26px tall and the numbered ones 22px, so
    // measuring each slot's own box drops the popup 2px lower for one of them.
    const UIElement* baseline = m_Row ? static_cast<const UIElement*>(m_Row) : static_cast<const UIElement*>(&slot);

    float x = (slot.GetLayoutX() - ax) + (slot.GetLayoutWidth() - kPopupWidth) * 0.5f;
    float y = (baseline->GetLayoutY() - ay) + baseline->GetLayoutHeight() + kSlotGap;

    // Centering a 128px popup on a 24px slot hangs it 52px to the slot's left,
    // which is off-window for the leftmost slot once the panel is maximized.
    // Clamp against the window, not the panel: the popup is an overlay and may
    // legitimately extend past its panel, but never past the window edge.
    UIManager* ui = GetOwnerManager();
    if (const UIElement* root = ui ? ui->GetRootElement() : nullptr)
    {
        const float minX = kWindowMargin - ax;
        const float maxX = root->GetLayoutWidth() - kWindowMargin - kPopupWidth - ax;
        x = std::clamp(x, minX, std::max(minX, maxX));

        const float minY = kWindowMargin - ay;
        const float maxY = root->GetLayoutHeight() - kWindowMargin - kPopupHeight - ay;
        y = std::clamp(y, minY, std::max(minY, maxY));
    }

    popup->Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(x))
        .Set(Style::PositionTop, StyleLength::Px(y));
}

void CameraBookmarksWidget::ShowSlotContextMenu(int index, float windowX, float windowY)
{
    Platform::Window* window = m_Panel ? m_Panel->GetWindow() : nullptr;
    if (!window)
        return;

    // The right-click ends the hover, so the preview goes with it rather than
    // lingering beside a menu the pointer has moved on to.
    ClosePreview();

    if (!m_ContextMenu)
    {
        m_ContextMenu = CreateContextMenu();
        if (!m_ContextMenu)
            return;
    }

    m_ContextMenu->SetCommandHandler([this, index](uint32_t cmd)
                                     {
        if (cmd == kCmdBookmarkReset)
            ResetDefaultBookmark();
        else if (cmd == kCmdBookmarkDelete && index >= 0)
            DeleteBookmark(static_cast<size_t>(index)); });

    m_ContextMenu->Clear();
    if (index == kDefaultSlotIndex)
    {
        m_ContextMenu->AddItem(0, "Reset Default View", kCmdBookmarkReset);
        m_ContextMenu->SetItemIcon(kCmdBookmarkReset, EditorIcons::kReset);
    }
    else
    {
        m_ContextMenu->AddItem(0, "Delete Bookmark", kCmdBookmarkDelete);
        m_ContextMenu->SetItemIcon(kCmdBookmarkDelete, EditorIcons::kTrash);
    }

    m_ContextMenu->Show(window, static_cast<int>(windowX), static_cast<int>(windowY));
}

void CameraBookmarksWidget::DeleteBookmark(size_t index)
{
    auto doDelete = [this, index]()
    {
        if (index >= m_Bookmarks.size())
            return;

        // close preview popup if it's open (and cancel any pending preview render)
        ClosePreview();

        m_Bookmarks.erase(m_Bookmarks.begin() + index);

        if (index < m_IsPreviewOpen.size())
            m_IsPreviewOpen.erase(m_IsPreviewOpen.begin() + index);

        if (m_IsPreviewOpen.size() != m_Bookmarks.size())
            m_IsPreviewOpen.assign(m_Bookmarks.size(), false);
        
        // Delete the preview image file if it exists
        std::filesystem::path previewPath = GetBookmarkPreviewPath(index);
        if (std::filesystem::exists(previewPath))
        {
            std::error_code ec;
            std::filesystem::remove(previewPath, ec);
        }
        
        // Save bookmarks after deletion
        SaveBookmarks();

        RebuildButtons();
    };

    if (IsInEventDispatch())
        PostAction(doDelete);
    else
        doDelete();
}

// ---- Default Bookmark (icon button, always visible) ----

void CameraBookmarksWidget::SaveDefaultBookmark()
{
    if (!m_Controller)
        return;

    m_DefaultBookmark.pose = m_Controller->GetCameraPose();
    m_DefaultBookmark.previewPixels.clear();
    m_DefaultBookmarkSet = true;
    
    // TODO: Request screenshot for default bookmark and save
    SaveBookmarks();
    UpdateActiveBookmarkVisual();
}

void CameraBookmarksWidget::RecallDefaultBookmark()
{
    if (!m_Controller || !m_Panel || !m_DefaultBookmarkSet)
        return;

    const auto& pose = m_DefaultBookmark.pose;

    // Don't switch projection immediately — let the tween finish and apply
    // the target pose (including Is2D) so the transition is visually smooth.
    m_Controller->StartCameraTween(pose, 0.7f);

    // update UI Panel camera for when the tween finishes
    m_Panel->SetYawPitch(pose.YawDeg, pose.PitchDeg);

    // Update the 2D/3D toolbar icon to reflect the bookmark's mode.
    if (auto* toolbar = dynamic_cast<SceneViewToolbar*>(GetParent()))
        toolbar->UpdateView2DButtonState();

    // Mark default bookmark as active (use -1 for default, numbered start at 0)
    m_ActiveBookmarkIndex = -2; // Special value for default bookmark
    UpdateActiveBookmarkVisual();
}

void CameraBookmarksWidget::ResetDefaultBookmark()
{
    m_DefaultBookmark = Bookmark{};
    m_DefaultBookmarkSet = false;
    SaveBookmarks();
    UpdateActiveBookmarkVisual();
}

void CameraBookmarksWidget::BindPreviewSnapshot()
{
    if (!m_Controller)
        return;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    // The read takes the device on purpose: a rebuild frees the snapshot's
    // generational slot while TextureHandle::IsValid() keeps answering true,
    // and the UI slot would then hold a freed id no validation layer flags.
    auto* renderServices = m_Controller->GetRenderServices();
    const Rendering::IDevice* device = renderServices ? renderServices->GetDevice() : nullptr;
    const auto snapshot = device ? m_Controller->GetPreviewSnapshotTexture(*device)
                                 : Rendering::TextureHandle{};
    if (!snapshot.IsValid())
    {
        // No readable snapshot ⇒ EVICT, never leave the last one bound. The
        // handle the slot holds was freed by whatever invalidated the read (a
        // device rebuild frees the slot while the id keeps testing valid), and
        // UIManager registers a dead handle rather than refusing it: its format
        // query answers Unknown, which is unclassifiable, not contradictory.
        // Clearing the latch is half the eviction — a stale `true` would keep
        // every re-bind site skipping the recovery.
        ui->RemoveExternalTexture(kPreviewTextureSlot);
        m_ExternalBound = false;
        return;
    }

    ui->SetExternalTexture(kPreviewTextureSlot, snapshot, kPreviewWidth, kPreviewHeight,
                           m_Controller->GetPreviewSnapshotSpace());
    m_ExternalBound = true;
}

void CameraBookmarksWidget::CaptureDefaultPreview(Button* b)
{
    if (!m_Controller || !m_Panel || !b)
        return;

    // Close any existing preview
    ClosePreview();
    
    if (!m_DefaultBookmarkSet)
        return; // No preview if bookmark not set
    
    m_ActivePreviewIndex = -2; // Special value for default bookmark
    m_ActivePreviewButtonId = b->GetId();

    // Position popup now, but gate visibility on a confirmed fresh preview render.
    if (auto* popup = m_Panel->GetPreviewPopup())
    {
        popup->RemoveClass("visible");

        PositionPreviewPopup(popup, *b);
    }
    m_WaitingForFreshPreview = true;
    m_WaitingPreviewFrames = 0;

    // Bind the frozen snapshot device texture once and reuse across hover
    // cycles (stable handle — content updates flow without rebinds). Invalid
    // until the first preview of a session completes; the ready re-bind in
    // Update() covers that.
    if (!m_ExternalBound)
        BindPreviewSnapshot();
    if (m_Controller)
        m_PendingPreviewSerial = m_Controller->RequestBookmarkPreview(m_DefaultBookmark.pose);
    EnsureRootMouseMoveFallback();
}

void CameraBookmarksWidget::CaptureScenePreview(const size_t index, Button* b)
{
    if (!m_Controller || !m_Panel || !b)
        return;
    if (index >= m_Bookmarks.size())
        return;

    if (m_IsPreviewOpen.size() != m_Bookmarks.size())
        m_IsPreviewOpen.assign(m_Bookmarks.size(), false);

    // Only one preview at a time. When switching slots, keep popup lifecycle alive
    // instead of full close/reopen to reduce hover churn.
    if (m_ActivePreviewIndex != static_cast<int>(index))
    {
        if (m_ActivePreviewIndex >= 0 && m_ActivePreviewIndex < static_cast<int>(m_IsPreviewOpen.size()))
            m_IsPreviewOpen[static_cast<size_t>(m_ActivePreviewIndex)] = false;
    }
    m_ActivePreviewIndex = static_cast<int>(index);
    m_ActivePreviewButtonId = b->GetId();

    m_IsPreviewOpen[index] = true;

    // Position popup now, but gate visibility on a confirmed fresh preview render.
    if (auto* popup = m_Panel->GetPreviewPopup())
    {
        // Hide until the new bookmark's render is ready. Without this, the popup
        // briefly shows the previous bookmark's content at the new position.
        popup->RemoveClass("visible");

        PositionPreviewPopup(popup, *b);
    }
    m_WaitingForFreshPreview = true;
    m_WaitingPreviewFrames = 0;

    // Bind the snapshot device texture (stable handle, see CaptureDefaultPreview).
    if (!m_ExternalBound)
        BindPreviewSnapshot();

    // Request preview render from this bookmark pose.
    if (m_Controller)
        m_PendingPreviewSerial = m_Controller->RequestBookmarkPreview(m_Bookmarks[index].pose);

    // Fallback: if MouseLeave is ever missed, close once the mouse target is no longer
    // within the active bookmark button.
    EnsureRootMouseMoveFallback();
}

void CameraBookmarksWidget::RemovePreview(size_t index)
{
    if (m_ActivePreviewIndex != static_cast<int>(index))
        return;
    ClosePreview();
}

bool CameraBookmarksWidget::IsPreviewPopupOpen() const
{
    return m_ActivePreviewIndex != -1;
}

void CameraBookmarksWidget::ClosePreview()
{
    RemoveRootMouseMoveFallback();
    m_WaitingForFreshPreview = false;
    m_PendingPreviewSerial = 0;
    m_WaitingPreviewFrames = 0;
    // Release the external texture binding when the popup closes. This prevents
    // stale RG handles from being sampled after tear-out/re-attach graph changes.
    if (UIManager* ui = GetOwnerManager())
    {
        ui->RemoveExternalTexture(kPreviewTextureSlot);
    }
    m_ExternalBound = false;

    // Hide popup.
    if (m_Panel)
    {
        if (auto* popup = m_Panel->GetPreviewPopup())
        {
            popup->RemoveClass("visible");
        }
    }

    // Cancel pending preview render + disable preview view submissions —
    // UNLESS a screenshot capture is still outstanding: Cancel closes the
    // serial gap, which would stop the old-arm frames before the readback
    // is ever enqueued (the '+'-while-popup-open lost-thumbnail bug). The
    // confirm path closes the serial itself once the capture renders.
    if (m_Controller && m_PendingScreenshotRequests.empty())
    {
        m_Controller->CancelBookmarkPreview();
    }

    // Clear per-slot state.
    if (m_ActivePreviewIndex >= 0 && m_ActivePreviewIndex < static_cast<int>(m_IsPreviewOpen.size()))
    {
        m_IsPreviewOpen[static_cast<size_t>(m_ActivePreviewIndex)] = false;
    }
    m_ActivePreviewIndex = -1;
    m_ActivePreviewButtonId.clear();
}

void CameraBookmarksWidget::EnsureRootMouseMoveFallback()
{
    // No-op. Preview closure is handled in Update() by querying pointer position.
}

void CameraBookmarksWidget::RemoveRootMouseMoveFallback()
{
    // Best-effort state clear only. Root elements can be destroyed during window
    // tear-out/re-attach; avoid dereferencing stale pointers here.
    m_RootMouseMoveToken = {};
    m_RootMouseMoveElement = nullptr;
}

Button* CameraBookmarksWidget::ResolveActivePreviewButton()
{
    if (!m_Row || m_ActivePreviewButtonId.empty())
        return nullptr;
    UIElement* el = m_Row->FindById(m_ActivePreviewButtonId);
    return dynamic_cast<Button*>(el);
}

void CameraBookmarksWidget::DisplayStoredPreview(size_t index)
{
    if (index >= m_Bookmarks.size())
        return;

    // Try to load preview pixels from the saved PNG file if not already loaded
    if (m_Bookmarks[index].previewPixels.empty())
    {
        std::filesystem::path previewPath = GetBookmarkPreviewPath(index);
        if (std::filesystem::exists(previewPath))
        {
            // Load PNG file
            int w, h, channels;
            unsigned char* data = stbi_load(previewPath.string().c_str(), &w, &h, &channels, 4);
            if (data && w == static_cast<int>(kPreviewWidth) && h == static_cast<int>(kPreviewHeight))
            {
                size_t pixelCount = static_cast<size_t>(w) * h * 4;
                m_Bookmarks[index].previewPixels.assign(data, data + pixelCount);
            }
            if (data)
                stbi_image_free(data);
        }
    }

    // If we have stored preview pixels, we need to create a texture from them
    // For now, we'll use the live preview system but request a render with the saved pose
    // This ensures the preview shows the correct view
    if (!m_Bookmarks[index].previewPixels.empty() && m_Controller)
    {
        BindPreviewSnapshot();
        m_Controller->RequestBookmarkPreview(m_Bookmarks[index].pose);
    }
    else if (m_Controller && !m_ExternalBound)
    {
        BindPreviewSnapshot();
    }
}

std::filesystem::path CameraBookmarksWidget::GetBookmarkPreviewPath(size_t index) const
{
    auto projectPaths = Editor::GetCurrentEditorProjectPaths();
    
    // Store preview images in .Editor/BookmarkPreviews/
    std::filesystem::path previewsDir = projectPaths.projectEditorRoot / "BookmarkPreviews";
    std::filesystem::create_directories(previewsDir);
    
    return previewsDir / ("bookmark_" + std::to_string(index) + ".png");
}

void CameraBookmarksWidget::SavePreviewImage(size_t index, uint32_t width, uint32_t height,
                                               Rendering::TextureFormat format)
{
    if (index >= m_Bookmarks.size() || m_Bookmarks[index].previewPixels.empty())
        return;
    if (width == 0 || height == 0)
        return;

    const auto& pixels = m_Bookmarks[index].previewPixels;
    const uint32_t bpp = Rendering::BytesPerPixel(format);
    const size_t expectedSize = static_cast<size_t>(width) * height * (bpp > 0 ? bpp : 4);
    if (pixels.size() < expectedSize)
        return;

    // Convert to RGBA8 if the readback is in a different format (e.g. float16 HDR).
    std::vector<uint8_t> rgba8;
    const void* pngSrc = pixels.data();

    if (format == Rendering::TextureFormat::R16G16B16A16_FLOAT)
    {
        const size_t pixelCount = static_cast<size_t>(width) * height;
        rgba8.resize(pixelCount * 4);
        const auto* src = reinterpret_cast<const uint16_t*>(pixels.data());

        auto halfToFloat = [](uint16_t h) -> float {
            uint32_t sign = (h >> 15) & 1;
            uint32_t exp = (h >> 10) & 0x1F;
            uint32_t mantissa = h & 0x3FF;
            if (exp == 0) return 0.0f;
            if (exp == 31) return sign ? -1.0f : 1.0f;
            float f = std::ldexp(static_cast<float>(mantissa) / 1024.0f + 1.0f, static_cast<int>(exp) - 15);
            return sign ? -f : f;
        };

        for (size_t i = 0; i < pixelCount; ++i)
        {
            float r = std::max(0.0f, std::min(1.0f, halfToFloat(src[i * 4 + 0])));
            float g = std::max(0.0f, std::min(1.0f, halfToFloat(src[i * 4 + 1])));
            float b = std::max(0.0f, std::min(1.0f, halfToFloat(src[i * 4 + 2])));
            rgba8[i * 4 + 0] = static_cast<uint8_t>(r * 255.0f);
            rgba8[i * 4 + 1] = static_cast<uint8_t>(g * 255.0f);
            rgba8[i * 4 + 2] = static_cast<uint8_t>(b * 255.0f);
            rgba8[i * 4 + 3] = 255;
        }
        pngSrc = rgba8.data();
    }

    std::filesystem::path previewPath = GetBookmarkPreviewPath(index);

    stbi_write_png(
        previewPath.string().c_str(),
        static_cast<int>(width),
        static_cast<int>(height),
        4,
        pngSrc,
        static_cast<int>(width * 4));
}

void CameraBookmarksWidget::SaveBookmarks()
{
    auto& engine = EngineCore::GetInstance();
    auto userSettings = Editor::OpenUserProjectSettings(engine.GetWorkspaceRoot());
    
    std::string err;
    userSettings.Load(&err);
    
    // Save default bookmark
    nlohmann::json defaultBmJson;
    defaultBmJson["isSet"] = m_DefaultBookmarkSet;
    if (m_DefaultBookmarkSet)
    {
        defaultBmJson["pose"]["pos"] = {m_DefaultBookmark.pose.Pos[0], m_DefaultBookmark.pose.Pos[1], m_DefaultBookmark.pose.Pos[2]};
        defaultBmJson["pose"]["yawDeg"] = m_DefaultBookmark.pose.YawDeg;
        defaultBmJson["pose"]["pitchDeg"] = m_DefaultBookmark.pose.PitchDeg;
        defaultBmJson["pose"]["distance"] = m_DefaultBookmark.pose.Distance;
        defaultBmJson["pose"]["is2D"] = m_DefaultBookmark.pose.Is2D;
    }
    userSettings.SetJson("defaultCameraBookmark", defaultBmJson);
    
    // Save bookmark poses as JSON array
    nlohmann::json bookmarksJson = nlohmann::json::array();
    for (size_t i = 0; i < m_Bookmarks.size(); ++i)
    {
        const auto& bm = m_Bookmarks[i];
        nlohmann::json bookmarkJson;
        bookmarkJson["pose"]["pos"] = {bm.pose.Pos[0], bm.pose.Pos[1], bm.pose.Pos[2]};
        bookmarkJson["pose"]["yawDeg"] = bm.pose.YawDeg;
        bookmarkJson["pose"]["pitchDeg"] = bm.pose.PitchDeg;
        bookmarkJson["pose"]["distance"] = bm.pose.Distance;
        bookmarkJson["pose"]["is2D"] = bm.pose.Is2D;
        bookmarkJson["previewPath"] = GetBookmarkPreviewPath(i).string();
        bookmarksJson.push_back(bookmarkJson);
    }
    
    userSettings.SetJson("cameraBookmarks", bookmarksJson);
    userSettings.Save(&err);
}

void CameraBookmarksWidget::LoadBookmarks()
{
    try
    {
        auto& engine = EngineCore::GetInstance();
        auto userSettings = Editor::OpenUserProjectSettings(engine.GetWorkspaceRoot());
        
        std::string err;
        if (!userSettings.Load(&err))
            return; // No settings file or error loading
        
        // Load default bookmark with error handling
        if (userSettings.Contains("defaultCameraBookmark"))
        {
            try
            {
                const auto& defaultBmJson = userSettings.Json()["defaultCameraBookmark"];
                if (defaultBmJson.is_object())
                {
                    m_DefaultBookmarkSet = defaultBmJson.value("isSet", false);
                    if (m_DefaultBookmarkSet && defaultBmJson.contains("pose") && defaultBmJson["pose"].is_object())
                    {
                        const auto& poseJson = defaultBmJson["pose"];
                        if (poseJson.contains("pos") && poseJson["pos"].is_array() && poseJson["pos"].size() == 3)
                        {
                            if (poseJson["pos"][0].is_number() && poseJson["pos"][1].is_number() && poseJson["pos"][2].is_number())
                            {
                                m_DefaultBookmark.pose.Pos[0] = poseJson["pos"][0].get<float>();
                                m_DefaultBookmark.pose.Pos[1] = poseJson["pos"][1].get<float>();
                                m_DefaultBookmark.pose.Pos[2] = poseJson["pos"][2].get<float>();
                            }
                        }
                        if (poseJson.contains("yawDeg") && poseJson["yawDeg"].is_number())
                            m_DefaultBookmark.pose.YawDeg = poseJson["yawDeg"].get<float>();
                        if (poseJson.contains("pitchDeg") && poseJson["pitchDeg"].is_number())
                            m_DefaultBookmark.pose.PitchDeg = poseJson["pitchDeg"].get<float>();
                        if (poseJson.contains("distance") && poseJson["distance"].is_number())
                            m_DefaultBookmark.pose.Distance = poseJson["distance"].get<float>();
                        if (poseJson.contains("is2D") && poseJson["is2D"].is_boolean())
                            m_DefaultBookmark.pose.Is2D = poseJson["is2D"].get<bool>();
                    }
                }
            }
            catch (...)
            {
                // If default bookmark loading fails, reset to defaults
                m_DefaultBookmarkSet = true; // Keep default initialized values
            }
        }
        
        if (!userSettings.Contains("cameraBookmarks"))
            return; // No bookmarks saved
        
        nlohmann::json bookmarksJson = userSettings.Json()["cameraBookmarks"];
        if (!bookmarksJson.is_array())
            return;
        
        m_Bookmarks.clear();
        m_IsPreviewOpen.clear();
        
        for (size_t i = 0; i < bookmarksJson.size() && i < MAX_BOOKMARKS; ++i)
        {
            try
            {
                const auto& bookmarkJson = bookmarksJson[i];
                if (!bookmarkJson.is_object())
                    continue; // Skip invalid entries
                    
                Bookmark bm{};
                
                // Load pose with validation
                if (bookmarkJson.contains("pose") && bookmarkJson["pose"].is_object())
                {
                    const auto& poseJson = bookmarkJson["pose"];
                    if (poseJson.contains("pos") && poseJson["pos"].is_array() && poseJson["pos"].size() == 3)
                    {
                        if (poseJson["pos"][0].is_number() && poseJson["pos"][1].is_number() && poseJson["pos"][2].is_number())
                        {
                            bm.pose.Pos[0] = poseJson["pos"][0].get<float>();
                            bm.pose.Pos[1] = poseJson["pos"][1].get<float>();
                            bm.pose.Pos[2] = poseJson["pos"][2].get<float>();
                        }
                    }
                    if (poseJson.contains("yawDeg") && poseJson["yawDeg"].is_number())
                        bm.pose.YawDeg = poseJson["yawDeg"].get<float>();
                    if (poseJson.contains("pitchDeg") && poseJson["pitchDeg"].is_number())
                        bm.pose.PitchDeg = poseJson["pitchDeg"].get<float>();
                    if (poseJson.contains("distance") && poseJson["distance"].is_number())
                        bm.pose.Distance = poseJson["distance"].get<float>();
                    if (poseJson.contains("is2D") && poseJson["is2D"].is_boolean())
                        bm.pose.Is2D = poseJson["is2D"].get<bool>();
                }

                // Load preview image if path exists
                if (bookmarkJson.contains("previewPath") && bookmarkJson["previewPath"].is_string())
                {
                    try
                    {
                        std::string previewPathStr = bookmarkJson["previewPath"].get<std::string>();
                        std::filesystem::path previewPath(previewPathStr);
                        if (std::filesystem::exists(previewPath))
                        {
                            // Load PNG file
                            int w, h, channels;
                            unsigned char* data = stbi_load(previewPath.string().c_str(), &w, &h, &channels, 4);
                            if (data && w == static_cast<int>(kPreviewWidth) && h == static_cast<int>(kPreviewHeight))
                            {
                                size_t pixelCount = static_cast<size_t>(w) * h * 4;
                                bm.previewPixels.assign(data, data + pixelCount);
                            }
                            if (data)
                                stbi_image_free(data);
                        }
                    }
                    catch (...)
                    {
                        // If preview loading fails, continue without preview
                    }
                }
                
                m_Bookmarks.push_back(bm);
                m_IsPreviewOpen.push_back(false);
            }
            catch (...)
            {
                // Skip corrupted bookmark entries
                continue;
            }
        }
        
        // Ensure vectors are in sync
        if (m_IsPreviewOpen.size() != m_Bookmarks.size())
            m_IsPreviewOpen.assign(m_Bookmarks.size(), false);
        
        // Resize pending readbacks vector
        m_PendingReadbacks.resize(m_Bookmarks.size());
    }
    catch (...)
    {
        // If loading completely fails, reset to defaults
        m_Bookmarks.clear();
        m_IsPreviewOpen.clear();
        m_PendingReadbacks.clear();
        m_DefaultBookmarkSet = true; // Keep default initialized values
    }
}

void CameraBookmarksWidget::Update()
{
    // Close the preview when the pointer is no longer over the anchor button or popup.
    if (m_ActivePreviewIndex != -1)
    {
        UIManager* ui = GetOwnerManager();
        if (!ui || !m_Panel)
        {
            ClosePreview();
        }
        else
        {
            const Mathematics::Vector2 cursor = ui->GetMousePosition();
            // Spurious WM_MOUSELEAVE on Windows can reset the position to the
            // unknown sentinel — don't close the preview when we can't tell
            // whether the mouse actually left the button.
            const bool mouseKnown = ui->IsMousePositionKnown();
            bool overButton = false;
            if (Button* activeButton = ResolveActivePreviewButton())
                overButton = mouseKnown && IsPointInside(*activeButton, cursor.x, cursor.y);
            UIElement* popup = m_Panel->GetPreviewPopup();
            const bool overPopup = mouseKnown && (popup != nullptr) && IsPointInside(*popup, cursor.x, cursor.y);
            if (mouseKnown && !overButton && !overPopup && !m_WaitingForFreshPreview)
                ClosePreview();
        }
    }

    // Show the popup once the frozen snapshot is bound. HasPreviewRenderForSerial
    // closes when the preview view submitted, which is before the copy pass
    // runs — binding then is how the slot picks up the new handle. Without a
    // handle the popup stays hidden rather than showing the CSS dummy fill.
    if (m_WaitingForFreshPreview && m_Panel && m_ActivePreviewIndex != -1 && m_Controller)
    {
        ++m_WaitingPreviewFrames;
        if (m_Controller->HasPreviewRenderForSerial(m_PendingPreviewSerial))
            BindPreviewSnapshot();
        if (m_ExternalBound)
        {
            if (auto* popup = m_Panel->GetPreviewPopup())
                popup->AddClass("visible");
            m_WaitingForFreshPreview = false;
            m_WaitingPreviewFrames = 0;
        }
        else if (m_WaitingPreviewFrames >= kPreviewSnapshotWaitFrames)
        {
            Logger::Log::Warning("[CameraBookmarks] preview snapshot wait timed out after {} frames",
                                 kPreviewSnapshotWaitFrames);
            m_WaitingForFreshPreview = false;
            m_WaitingPreviewFrames = 0;
        }
    }
    UpdateReadbacks();
}

void CameraBookmarksWidget::RequestScreenshotForBookmark(size_t index)
{
    if (index >= m_Bookmarks.size() || !m_Controller)
        return;
    
    // Request preview render - this will render the scene from the bookmark's pose
    m_Controller->RequestBookmarkPreview(m_Bookmarks[index].pose);
    
    // Mark this bookmark as needing a screenshot
    // The readback will be requested after the preview renders (handled elsewhere)
    if (std::find(m_PendingScreenshotRequests.begin(), m_PendingScreenshotRequests.end(), index) == m_PendingScreenshotRequests.end())
    {
        m_PendingScreenshotRequests.push_back(index);
    }
    
    // Ensure readback request vector is large enough
    if (m_PendingReadbacks.size() <= index)
        m_PendingReadbacks.resize(index + 1);
}

bool CameraBookmarksWidget::ProcessPendingScreenshotRequestsRG(
    GameEngine::Rendering::RenderGraph::RGFrame& frame)
{
    if (!m_Controller || m_PendingScreenshotRequests.empty())
        return false;

    // Request ticket readbacks for pending screenshots — called post-spine
    // on frames where the preview rendered, so the pipeline output exists.
    for (size_t index : m_PendingScreenshotRequests)
    {
        if (index >= m_Bookmarks.size())
            continue;

        if (index < m_PendingReadbacks.size() && m_PendingReadbacks[index])
            continue; // Already have a pending readback

        auto ticket = m_Controller->RequestPreviewReadbackRG(frame);
        if (ticket)
        {
            if (m_PendingReadbacks.size() <= index)
                m_PendingReadbacks.resize(index + 1);
            m_PendingReadbacks[index] = std::move(ticket);
        }
    }

    return !m_PendingScreenshotRequests.empty();
}

void CameraBookmarksWidget::UpdateReadbacks()
{
    if (!m_Controller || !m_Panel)
        return;

    // Poll readback tickets and store pixels when stamped + GPU-signaled.
    for (size_t i = 0; i < m_PendingReadbacks.size() && i < m_Bookmarks.size(); ++i)
    {
        if (!m_PendingReadbacks[i])
            continue;

        // A cancelled ticket (declaring frame abandoned before submit) can
        // never resolve — drop it; the request index stays pending and the
        // next rendered preview frame re-arms it.
        if (m_PendingReadbacks[i]->IsConsumed())
        {
            m_PendingReadbacks[i] = nullptr;
            continue;
        }

        Rendering::ViewReadbackResult result;
        if (m_PendingReadbacks[i]->TryGet(result))
        {
            m_Bookmarks[i].previewPixels = std::move(result.pixels);
            m_PendingReadbacks[i] = nullptr;
            SavePreviewImage(i, result.width, result.height, result.format);

            // Save bookmarks (including the new preview path)
            SaveBookmarks();

            // Remove from pending requests
            m_PendingScreenshotRequests.erase(
                std::remove(m_PendingScreenshotRequests.begin(), m_PendingScreenshotRequests.end(), i),
                m_PendingScreenshotRequests.end());
        }
    }

    // NO compaction: this vector is indexed BY BOOKMARK INDEX everywhere
    // else (ProcessPendingScreenshotRequestsRG, the poll loop above) —
    // erasing completed entries shifts later in-flight readbacks onto the
    // wrong bookmarks. Completed slots are reset in place to null.
}

namespace RegisterWidgets
{
static auto s_reg_cameraBookmarks =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::CameraBookmarksWidget>(
        "CameraBookmarksWidget",
        []() { return std::make_unique<GameEngine::CameraBookmarksWidget>(); })
        .TagAlias("camerabookmarkswidget");
}

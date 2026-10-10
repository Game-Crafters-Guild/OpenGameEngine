#pragma once

#include "AssetCore/GUID.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/UIElement.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace GameEngine
{

class AssetRegistry;
namespace UI
{
class IUiDispatcher;
}

/// Inspector thumbnail for a material texture slot: shows the assigned texture and
/// accepts the same asset-path drag payload as \ref AssetField for quick reassignment.
class MaterialTexturePreviewHost : public UIElement, public UI::Interaction::IDropTarget
{
public:
    using TextureDroppedCallback = std::function<void(const GUID& guid)>;
    using TextureClickedCallback = std::function<void(const GUID& guid)>;

    MaterialTexturePreviewHost();
    ~MaterialTexturePreviewHost() override;

    void SetAssetRegistry(AssetRegistry* registry) { m_Registry = registry; }
    void SetOnTextureDropped(TextureDroppedCallback callback) { m_OnTextureDropped = std::move(callback); }
    void SetOnTextureClicked(TextureClickedCallback callback) { m_OnTextureClicked = std::move(callback); }

    void SetTexturePreviewFromGuid(const AssetRegistry& registry, const GUID& guid);
    void OnEvent(UIEvent& e) override;

    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

private:
    // Try to resolve `m_CurrentTextureGuid` against the registry and apply
    // the texture's path as the background. Returns true if the registry
    // had the metadata and the preview was applied; false if the metadata
    // is missing (preview goes empty and an asset-event subscription is
    // installed so we retry once the texture registers).
    bool TryApplyPreview();

    void SubscribeToAssetEvents();
    void UnsubscribeFromAssetEvents();
    void RetryPreviewAfterAssetEvent();
    void OnOwnerManagerChanged(UIManager* owner) override;

    // What an asset-event callback may touch. The dispatcher runs callbacks on the
    // thread that raised the event (the file watcher's, for a newly registered
    // texture), so the callback reads only this, under its mutex, and posts the
    // retry to the owning UI manager's dispatcher. The UI thread writes it: the
    // dispatcher when the host is attached to or detached from a manager, and
    // Host cleared before the host goes. An event that arrives while the host has
    // no manager leaves RetryPending set, and attaching posts the retry.
    struct AssetEventBridge
    {
        std::mutex Mutex;
        MaterialTexturePreviewHost* Host = nullptr;
        UI::IUiDispatcher* Dispatcher = nullptr;
        GUID TextureGuid = GUID::Null();
        bool RetryPending = false;
    };
    // Posts the retry to `bridge`'s dispatcher, or marks it pending when there is
    // none. Called with the bridge's mutex held.
    static void PostRetryLocked(const std::shared_ptr<AssetEventBridge>& bridge);

    AssetRegistry* m_Registry = nullptr;
    GUID m_CurrentTextureGuid = GUID::Null();
    TextureDroppedCallback m_OnTextureDropped;
    TextureClickedCallback m_OnTextureClicked;
    bool m_ClickArmed = false;
    // 0 means no active subscription. Asset-event dispatcher hands out
    // monotonic positive uint32 handles for `AddCallback`.
    uint32_t m_AssetEventHandle = 0;
    std::shared_ptr<AssetEventBridge> m_AssetEventBridge;
};

} // namespace GameEngine

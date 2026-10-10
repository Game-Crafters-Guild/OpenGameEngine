#include "UI/MaterialTexturePreviewHost.h"

#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Editor/DragDropPayloads.h"
#include "UI/Interaction/Payload.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UiDispatcher.h"

#include <filesystem>

namespace GameEngine
{

MaterialTexturePreviewHost::MaterialTexturePreviewHost() = default;

MaterialTexturePreviewHost::~MaterialTexturePreviewHost()
{
    UnsubscribeFromAssetEvents();
}

void MaterialTexturePreviewHost::SetTexturePreviewFromGuid(const AssetRegistry& registry, const GUID& guid)
{
    m_CurrentTextureGuid = guid;
    if (m_Registry != &registry)
        m_Registry = const_cast<AssetRegistry*>(&registry);

    if (TryApplyPreview())
    {
        UnsubscribeFromAssetEvents();
        return;
    }

    // Metadata not in the registry yet. Common when a project-load asset
    // scan is still in flight and the user clicks a material whose
    // textures haven't been indexed. Subscribe to asset events so the
    // preview retries once the texture registers — clicking around the
    // inspector usually triggers a re-evaluation eventually, but this
    // makes it self-healing without user interaction.
    SubscribeToAssetEvents();
}

bool MaterialTexturePreviewHost::TryApplyPreview()
{
    RemoveClass("texture-preview-empty");
    if (m_CurrentTextureGuid.IsNull())
    {
        AddClass("texture-preview-empty");
        UI::Layout::SetBackgroundPath(*this, "");
        return true; // null guid is a "successful" resolution (nothing to show)
    }
    if (!m_Registry)
    {
        AddClass("texture-preview-empty");
        UI::Layout::SetBackgroundPath(*this, "");
        return false;
    }
    AssetMetadata metadata{};
    if (!m_Registry->TryGetAssetMetadata(m_CurrentTextureGuid, metadata) || metadata.Path.empty())
    {
        AddClass("texture-preview-empty");
        UI::Layout::SetBackgroundPath(*this, "");
        return false;
    }
    UI::Layout::SetBackgroundPath(*this, metadata.Path.string());
    return true;
}

void MaterialTexturePreviewHost::SubscribeToAssetEvents()
{
    if (m_AssetEventHandle != 0)
    {
        std::lock_guard<std::mutex> lock(m_AssetEventBridge->Mutex);
        m_AssetEventBridge->TextureGuid = m_CurrentTextureGuid;
        return;
    }
    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;
    m_AssetEventBridge = std::make_shared<AssetEventBridge>();
    m_AssetEventBridge->Host = this;
    m_AssetEventBridge->TextureGuid = m_CurrentTextureGuid;
    if (UIManager* owner = GetOwnerManager())
        m_AssetEventBridge->Dispatcher = owner->GetDispatcher();
    // The callback runs on the thread that raised the event: a texture that
    // registers through a scan or the file watcher raises AssetCreated or
    // AssetModified there. It reads only the bridge, under its mutex, and posts
    // the retry to the dispatcher the UI thread stored there; the host itself is
    // touched only by the posted retry, on the UI thread. The dispatcher's own
    // mutex orders nothing here: DispatchEvent invokes copies of the callbacks
    // after releasing it.
    m_AssetEventHandle = engine.GetAssetManager().GetEventDispatcher().AddCallback(
        [bridge = m_AssetEventBridge](const AssetEvent& event)
        {
            if (event.Type != AssetType::Texture)
                return;
            std::lock_guard<std::mutex> lock(bridge->Mutex);
            if (!bridge->Host || event.AssetGuid != bridge->TextureGuid)
                return;
            PostRetryLocked(bridge);
        });
}

void MaterialTexturePreviewHost::PostRetryLocked(const std::shared_ptr<AssetEventBridge>& bridge)
{
    if (!bridge->Dispatcher)
    {
        bridge->RetryPending = true;
        return;
    }
    bridge->RetryPending = false;
    bridge->Dispatcher->Post(
        [bridge]
        {
            if (bridge->Host)
                bridge->Host->RetryPreviewAfterAssetEvent();
        });
}

void MaterialTexturePreviewHost::OnOwnerManagerChanged(UIManager* owner)
{
    UIElement::OnOwnerManagerChanged(owner);
    if (!m_AssetEventBridge)
        return;
    std::lock_guard<std::mutex> lock(m_AssetEventBridge->Mutex);
    m_AssetEventBridge->Dispatcher = owner ? owner->GetDispatcher() : nullptr;
    if (m_AssetEventBridge->RetryPending)
        PostRetryLocked(m_AssetEventBridge);
}

void MaterialTexturePreviewHost::RetryPreviewAfterAssetEvent()
{
    if (m_AssetEventHandle != 0 && TryApplyPreview())
        UnsubscribeFromAssetEvents();
}

void MaterialTexturePreviewHost::UnsubscribeFromAssetEvents()
{
    if (m_AssetEventHandle == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(m_AssetEventBridge->Mutex);
        m_AssetEventBridge->Host = nullptr;
        m_AssetEventBridge->Dispatcher = nullptr;
    }
    m_AssetEventBridge.reset();
    auto& engine = GameEngine::EngineCore::GetInstance();
    if (engine.IsInitialized())
    {
        engine.GetAssetManager().GetEventDispatcher().RemoveCallback(m_AssetEventHandle);
    }
    m_AssetEventHandle = 0;
}

void MaterialTexturePreviewHost::OnEvent(UIEvent& e)
{
    if (e.Id == kEventMouseDown && e.Button == 0)
    {
        if (ContainsPoint(e.X, e.Y))
        {
            m_ClickArmed = true;
            e.Capture(this);
            e.Stop();
            return;
        }
    }
    else if (e.Id == kEventMouseUp && e.Button == 0)
    {
        const bool inside = ContainsPoint(e.X, e.Y);
        if (m_ClickArmed)
        {
            m_ClickArmed = false;
            if (inside && m_OnTextureClicked && !m_CurrentTextureGuid.IsNull())
                m_OnTextureClicked(m_CurrentTextureGuid);
            e.Stop();
            return;
        }
    }

    UIElement::OnEvent(e);
}

bool MaterialTexturePreviewHost::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>();
}

bool MaterialTexturePreviewHost::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    if (!ContainsPoint(x, y))
        return false;
    out.TargetId = 0;
    out.Location = UI::Interaction::DropLocation::OnItem;
    return true;
}

UI::Interaction::DropFeedback MaterialTexturePreviewHost::CanDrop(const UI::Interaction::DropRequest& request) const
{
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload || payload->paths.empty())
        return {false, "No payload"};

    const std::filesystem::path path = payload->paths[0];
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec))
        return {false, "Missing file"};
    if (std::filesystem::is_directory(path, ec))
        return {false, "Drop a file, not a folder"};

    const std::string ext = path.extension().string();
    if (!IsValidExtensionForAssetType(AssetType::Texture, ext))
        return {false, "Expected Texture asset"};

    return {true, {}};
}

void MaterialTexturePreviewHost::PerformDrop(const UI::Interaction::DropRequest& request)
{
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload || payload->paths.empty() || !m_Registry)
        return;

    const std::filesystem::path path = payload->paths[0];
    const auto result = m_Registry->RegisterAssetByPath(path);
    if (!result.IsOk())
        return;

    if (m_OnTextureDropped)
        m_OnTextureDropped(result.Value());
}

void MaterialTexturePreviewHost::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    if (!state.Visible)
    {
        RemoveClass("drop-valid");
        RemoveClass("drop-invalid");
        return;
    }
    if (state.Allowed)
    {
        AddClass("drop-valid");
        RemoveClass("drop-invalid");
    }
    else
    {
        RemoveClass("drop-valid");
        AddClass("drop-invalid");
    }
}

} // namespace GameEngine

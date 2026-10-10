#include "Graph/GraphPortDropSlot.h"

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AssetSearchProvider.h"
#include "Core/Engine.h"
#include "Editor/DragDropPayloads.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphNodeSummary.h"
#include "Input/KeyCodes.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Interaction/Payload.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace GameEngine {
namespace {

std::string TextureSamplerNameFromPath(const std::filesystem::path& path)
{
    std::string source = path.stem().string();
    std::string name;
    name.reserve(source.size() + 4);
    for (unsigned char c : source)
    {
        if (std::isalnum(c) || c == '_')
            name.push_back(static_cast<char>(c));
        else if (name.empty() || name.back() != '_')
            name.push_back('_');
    }

    while (!name.empty() && name.back() == '_')
        name.pop_back();
    if (name.empty())
        name = "texture";
    if (std::isdigit(static_cast<unsigned char>(name.front())))
        name.insert(0, "tex_");
    return name;
}

} // namespace

namespace {

/* Graph files travel with the project: store asset-root-relative paths so a
   moved or shared project still resolves them (asset-path resolution handles
   the join). Paths outside the asset root stay as given. */
std::string ProjectRelativeTexturePath(const std::string& texturePath)
{
    const std::filesystem::path assetRoot =
        EngineCore::GetInstance().GetAssetManager().GetAssetRoot();
    if (assetRoot.empty())
        return texturePath;
    /* Key-domain containment: a path picked from the registry is folded while
       the root carries the caller's spelling, so a lexical subtraction compares
       case and yields nothing — which would store the absolute path the comment
       above says never to store. */
    std::string relative;
    if (!AssetRegistry::TryComputeCanonicalRelativePath(assetRoot, texturePath, relative))
        return texturePath;
    return relative;
}

} // namespace

GraphPortDropSlot::GraphPortDropSlot()
{
    AddClass("graph-port-drop-slot");
    SetFocusable(false);
    /* The slot claims its primary press (the same consumption every editor
       surface uses), so the canvas never turns it into a node drag; the picker
       itself opens on mouse-up so the release cannot clear focus from the
       dialog's search field the moment it appears. */
    RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
    {
        if (e.Button != Input::kMouseButton_Left || !m_AssetRegistry || m_NodeId.empty())
            return;
        if (e.CurrentTarget)
            e.Capture(e.CurrentTarget);
        e.Stop();
    });
    RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
    {
        if (e.Button != Input::kMouseButton_Left || !m_AssetRegistry || m_NodeId.empty())
            return;
        e.Stop();
        OpenTexturePicker();
    });
}

GraphPortDropSlot::~GraphPortDropSlot()
{
    CloseTexturePicker();
}

void GraphPortDropSlot::SetPickerServices(AssetRegistry* registry, IThumbnailProvider* thumbnails)
{
    m_AssetRegistry = registry;
    m_Thumbnails = thumbnails;
}

void GraphPortDropSlot::AssignTexture(const std::string& texturePathIn)
{
    if (m_NodeId.empty() || !m_Model)
        return;
    const std::string texturePath = ProjectRelativeTexturePath(texturePathIn);
    const std::string nodeId = m_NodeId;
    const std::string samplerName =
        TextureSamplerNameFromPath(std::filesystem::path(texturePath));
    Graph::Model* model = m_Model;
    auto onChanged = m_OnChanged;
    auto mutate = [model, nodeId, texturePath, samplerName, onChanged]()
    {
        Graph::Node* node = model ? model->FindNode(nodeId) : nullptr;
        if (!node || !IsTextureValueNode(*node))
            return;
        node->Parameters["texture"] = samplerName;
        node->Parameters["texturePath"] = texturePath;
        if (onChanged)
            onChanged();
    };
    if (m_Undo)
        m_Undo("Assign Texture", std::move(mutate));
    else
        mutate();
}

void GraphPortDropSlot::CloseTexturePicker()
{
    SearchDialog* dialog = m_ActiveDialog;
    m_ActiveDialog = nullptr;
    if (!dialog)
    {
        m_SearchProvider.reset();
        return;
    }
    if (UIManager* mgr = GetOwnerManager())
    {
        if (UIElement* root = mgr->GetRootElement())
            root->RemoveChild(dialog);
    }
    m_SearchProvider.reset();
}

void GraphPortDropSlot::OpenTexturePicker()
{
    if (m_ActiveDialog || !m_AssetRegistry)
        return;
    UIManager* mgr = GetOwnerManager();
    UIElement* root = mgr ? mgr->GetRootElement() : nullptr;
    if (!root)
        return;
    if (m_Model)
    {
        const Graph::Node* node = m_Model->FindNode(m_NodeId);
        if (!node || !IsTextureValueNode(*node))
            return;
    }

    auto provider = std::make_unique<AssetSearchProvider>(m_AssetRegistry);
    provider->SetTypeFilter({AssetType::Texture});
    provider->SetShowFullPath(true);
    if (m_Thumbnails)
        provider->SetThumbnailProvider(m_Thumbnails);
    m_SearchProvider = std::move(provider);

    auto dialog = std::make_unique<SearchDialog>();
    dialog->SetProvider(m_SearchProvider.get());
    SearchDialog* dialogPtr = dialog.get();
    m_ActiveDialog = dialogPtr;

    dialog->SetOnResult([this](const SearchResultItem& item)
    {
        /* Guard the registry: the slot can be rebound (picker services
           cleared) while the dialog is still up. */
        if (m_AssetRegistry)
        {
            if (const auto* guid = std::any_cast<GUID>(&item.UserData))
            {
                AssetMetadata meta;
                if (m_AssetRegistry->TryGetAssetMetadata(*guid, meta) && !meta.Path.empty())
                    AssignTexture(meta.Path.string());
            }
        }
        CloseTexturePicker();
    });
    dialog->SetOnCancel([this]() { CloseTexturePicker(); });

    root->AddChild(std::move(dialog));
    dialogPtr->SetAnchorPosition(GetLayoutX() + GetLayoutWidth(),
                                 GetLayoutY() + GetLayoutHeight(),
                                 SearchDialogHorizontalAnchor::TrailingRight);
    dialogPtr->Show();
}

UI::Interaction::ItemId GraphPortDropSlot::HashNodeId(const std::string& nodeId)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : nodeId)
    {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    return hash != 0 ? hash : 1ull;
}

void GraphPortDropSlot::Bind(
    const std::string& nodeId,
    Graph::Model* model,
    std::function<void(const std::string& name, std::function<void()> mutate)> undo,
    std::function<void()> onChanged)
{
    m_NodeId = nodeId;
    m_Model = model;
    m_Undo = std::move(undo);
    m_OnChanged = std::move(onChanged);
}

void GraphPortDropSlot::Unbind()
{
    /* A dialog opened for the old binding must not assign into whatever node
       this recycled slot binds next. */
    CloseTexturePicker();
    m_NodeId.clear();
    m_Model = nullptr;
    m_Undo = {};
    m_OnChanged = {};
}

void GraphPortDropSlot::Reset()
{
    Unbind();
    RemoveClass("drop-valid");
    RemoveClass("drop-invalid");
    UI::Layout::ClearBackgroundOverride(*this);
    UI::Layout::SetElementHidden(*this, true);
}

bool GraphPortDropSlot::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>();
}

bool GraphPortDropSlot::HitTestDropTarget(float, float, UI::Interaction::DropHit& out) const
{
    if (m_NodeId.empty())
        return false;
    out.TargetId = HashNodeId(m_NodeId);
    out.Location = UI::Interaction::DropLocation::OnItem;
    return true;
}

UI::Interaction::DropFeedback GraphPortDropSlot::CanDrop(const UI::Interaction::DropRequest& request) const
{
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload || payload->paths.empty())
        return {false, "No image"};

    if (!m_Model)
        return {false, "Drop on a texture value"};

    const Graph::Node* node = m_Model->FindNode(m_NodeId);
    if (!node || !IsTextureValueNode(*node))
        return {false, "Drop on a texture value"};

    const std::filesystem::path path = payload->paths[0];
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec))
        return {false, "Missing file"};
    if (std::filesystem::is_directory(path, ec))
        return {false, "Drop an image file"};

    const std::string ext = path.extension().string();
    if (!IsValidExtensionForAssetType(AssetType::Texture, ext))
        return {false, "Expected image texture"};

    return {true, {}, false, "external"};
}

void GraphPortDropSlot::PerformDrop(const UI::Interaction::DropRequest& request)
{
    const auto* payload = request.payload.TryGet<Editor::AssetPathsDragPayload>();
    if (!payload || payload->paths.empty() || !m_Model || m_NodeId.empty())
        return;

    const auto feedback = CanDrop(request);
    if (!feedback.Allowed)
        return;

    AssignTexture(payload->paths[0].string());
}

void GraphPortDropSlot::SetDropPreview(const UI::Interaction::DropPreviewState& state)
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

namespace RegisterGraphElements
{
static auto s_reg_graphPortDropSlot =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::GraphPortDropSlot>(
        "GraphPortDropSlot",
        []() { return std::make_unique<GameEngine::GraphPortDropSlot>(); })
        .TagAlias("graphportdropslot");
}

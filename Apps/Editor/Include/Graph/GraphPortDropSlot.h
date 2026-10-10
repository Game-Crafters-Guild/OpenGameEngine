#pragma once

#include "UI/Interaction/DropTarget.h"
#include "UI/UIElement.h"

#include <functional>
#include <memory>
#include <string>

namespace GameEngine {

class AssetRegistry;
struct ISearchProvider;
class SearchDialog;
class IThumbnailProvider;

namespace Graph {
struct Model;
}

/** Texture-node drop target. Identity is HashNodeId(node.Id), never a vector index. */
class GraphPortDropSlot : public UIElement, public UI::Interaction::IDropTarget {
public:
    GraphPortDropSlot();
    ~GraphPortDropSlot() override;

    static UI::Interaction::ItemId HashNodeId(const std::string& nodeId);

    void Bind(const std::string& nodeId,
              Graph::Model* model,
              std::function<void(const std::string& name, std::function<void()> mutate)> undo,
              std::function<void()> onChanged);
    void Unbind();
    void Reset();

    /** Enables click-to-pick: a click opens a texture search dialog (same
        provider the inspector asset fields use). Set at every bind. */
    void SetPickerServices(AssetRegistry* registry, IThumbnailProvider* thumbnails);

    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

private:
    /** Shared commit for picker and drag-drop: stores the sampler name and the
        asset-root-relative path in one undo-scoped mutate. */
    void AssignTexture(const std::string& texturePath);
    void OpenTexturePicker();
    void CloseTexturePicker();

    std::string m_NodeId;
    Graph::Model* m_Model = nullptr;
    std::function<void(const std::string& name, std::function<void()> mutate)> m_Undo;
    std::function<void()> m_OnChanged;

    AssetRegistry* m_AssetRegistry = nullptr;
    IThumbnailProvider* m_Thumbnails = nullptr;
    SearchDialog* m_ActiveDialog = nullptr;
    std::unique_ptr<ISearchProvider> m_SearchProvider;
};

} // namespace GameEngine

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AssetCore/AssetTypes.h"
#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "UI/ReferenceFieldBase.h"

namespace GameEngine
{

class AssetRegistry;
class AssetSearchProvider;
class IThumbnailProvider;
struct AssetIndexRecord;

/// Inspector-friendly field for selecting an asset by GUID.
///
/// Displays the current asset (type icon + name) and supports:
///   - Click to open a SearchDialog filtered by accepted types
///   - Drag-drop of AssetPathsDragPayload
///   - Clear button to remove the reference
///
/// Usage:
///   auto field = std::make_unique<AssetField>();
///   field->SetAcceptedTypes({AssetType::Model});
///   field->SetAssetRegistry(&registry);
///   field->SetValue(currentGuid);
///   field->SetOnValueChanged([](const GUID& g) { ... });
///   parent->AddChild(std::move(field));
class AssetField : public ReferenceFieldBase
{
public:
    using ValueChangedCallback = std::function<void(const GUID& newValue)>;

    AssetField();

    /// Restrict which asset types this field accepts (e.g. {AssetType::Model}).
    void SetAcceptedTypes(std::vector<AssetType> types);

    /// Further restrict accepted/searchable assets by metadata.
    void SetMetadataFilter(std::function<bool(const AssetIndexRecord&)> filter);

    /// Provide access to the asset registry for metadata lookup and search.
    void SetAssetRegistry(AssetRegistry* registry);

    /// Optional thumbnail provider for image previews in the search dropdown.
    void SetThumbnailProvider(IThumbnailProvider* provider) { m_Thumbnails = provider; }

    /// Set the current value. Updates the display (icon + name).
    void SetValue(const GUID& guid);
    const GUID& GetValue() const { return m_Value; }

    /// Clear the current value to null GUID.
    void Clear();

    /// Callback invoked when the value changes (selection, drop, or clear).
    void SetOnValueChanged(ValueChangedCallback cb) { m_OnValueChanged = std::move(cb); }

    // -- IDropTarget: payload-specific parts (generic hit-test/preview in base) --
    bool AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const override;
    UI::Interaction::DropFeedback CanDrop(const UI::Interaction::DropRequest& request) const override;
    void PerformDrop(const UI::Interaction::DropRequest& request) override;

protected:
    std::unique_ptr<ISearchProvider> CreateProvider() override;
    void OnResultSelected(const SearchResultItem& item) override;
    void PositionDialog(SearchDialog& dialog) override;
    SearchItemId CurrentResultId() const override;
    void OnClearRequested() override { Clear(); }

private:
    void UpdateDisplay();
    void ClearModelPreviewStrip();
    void ApplyValue(const GUID& guid);
    bool IsAcceptedExtension(const std::string& ext) const;

    GUID m_Value;
    std::vector<AssetType> m_AcceptedTypes;
    std::function<bool(const AssetIndexRecord&)> m_MetadataFilter;
    AssetRegistry* m_Registry = nullptr;
    IThumbnailProvider* m_Thumbnails = nullptr;
    ValueChangedCallback m_OnValueChanged;

    // Asset-specific chrome (raw pointers; owned by the UIElement children vector).
    UIElement* m_Icon = nullptr;
    UIElement* m_Preview = nullptr;

    // Current icon CSS class applied to m_Icon (empty when hidden).
    std::string m_CurrentIconClass;

    uint32_t m_ModelPreviewRequestGen = 0;
};

} // namespace GameEngine

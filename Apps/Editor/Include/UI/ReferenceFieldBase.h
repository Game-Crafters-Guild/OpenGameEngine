#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "UI/UIElement.h"
#include "UI/Interaction/DropTarget.h"

namespace GameEngine
{

class Label;
class SearchDialog;
struct ISearchProvider;
struct SearchResultItem;
using SearchItemId = uint64_t;

/// Shared base for inspector "reference picker" fields (AssetField, EntityField):
/// a bordered name label + clear button that opens a SearchDialog and accepts a
/// drag-drop payload. Owns the dialog lifetime, the destructor teardown, the
/// generic drop hit-test / preview classes, the name+clear structure, and the
/// empty/named display toggle. Subclasses own the value type, the payload decode
/// + validation, any extra chrome (e.g. an icon/preview), the search provider,
/// result handling, and dialog placement.
class ReferenceFieldBase : public UIElement, public UI::Interaction::IDropTarget
{
public:
    ReferenceFieldBase();
    ~ReferenceFieldBase() override;

    // -- IDropTarget: generic parts (payload-specific overrides stay pure virtual) --
    bool HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const override;
    void SetDropPreview(const UI::Interaction::DropPreviewState& state) override;

    /// Whether the field offers its clear button. Turn it off for a set-only
    /// reference — one whose null value is a state the user cannot author, so a
    /// clear would be an edit that something else silently undoes.
    void SetClearable(bool clearable);

protected:
    // Create the name label + clear button and wire their handlers (clear button
    // -> OnClearRequested; field click -> OpenReferenceDialog). Call from the
    // subclass constructor AFTER adding any leading children (e.g. an icon) so the
    // child/paint order is preserved.
    void BuildNameAndClearButton();

    // Open the shared search dialog. No-op when one is already open or the subclass
    // is not ready (CreateProvider() returned null).
    void OpenReferenceDialog();

    // Name/clear display toggles for subclass UpdateDisplay().
    void ShowEmpty();                        // "(None)", empty class, clear hidden
    void ShowNamed(const std::string& name); // name, remove empty, clear shown

    // -- Subclass hooks --
    virtual std::unique_ptr<ISearchProvider> CreateProvider() = 0;
    virtual void OnResultSelected(const SearchResultItem& item) = 0;
    virtual void PositionDialog(SearchDialog& dialog) = 0;
    // Result id of the field's current value, so the dialog opens highlighting
    // it. 0 when the field is empty.
    virtual SearchItemId CurrentResultId() const = 0;
    virtual void OnClearRequested() = 0;

    Label* m_NameLabel = nullptr;
    UIElement* m_ClearBtn = nullptr;
    bool m_Clearable = true;

private:
    std::unique_ptr<ISearchProvider> m_SearchProvider; // kept alive while the dialog is open
    SearchDialog* m_ActiveDialog = nullptr;
};

} // namespace GameEngine

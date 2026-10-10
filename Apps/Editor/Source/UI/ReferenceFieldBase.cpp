#include "UI/ReferenceFieldBase.h"

#include "UI/Controls/Label.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

namespace GameEngine
{

ReferenceFieldBase::ReferenceFieldBase()
{
    // Both reference fields share the asset-field stylesheet (bordered name +
    // clear button + drop-valid/invalid states).
    AddClass("asset-field");
    RequestSubtreeStyleAssetPath("UI/controls/AssetField.css", "editor");
}

ReferenceFieldBase::~ReferenceFieldBase()
{
    // Tear down a still-open dialog. By now m_OwnerManager has been cleared
    // (RemoveChild -> SetOwnerManager(nullptr)), so reach the root via the
    // dialog's parent -- it was added directly to the root element.
    if (m_ActiveDialog)
    {
        if (UIElement* parent = m_ActiveDialog->GetParent())
            parent->RemoveChild(m_ActiveDialog);
        m_ActiveDialog = nullptr;
    }
    m_SearchProvider.reset();
}

void ReferenceFieldBase::BuildNameAndClearButton()
{
    auto name = std::make_unique<Label>();
    name->AddClass("asset-field-name");
    name->AddClass("empty");
    name->SetText("(None)");
    m_NameLabel = name.get();
    AddChild(std::move(name));

    auto clearBtn = std::make_unique<UIElement>();
    clearBtn->AddClass("asset-field-clear");
    clearBtn->AddClass("hidden");
    clearBtn->SetTooltip("Clear");
    m_ClearBtn = clearBtn.get();
    AddChild(std::move(clearBtn));

    // Clear on mouse-up (not mouse-down): clearing hides the button, so a
    // mouse-down clear would leave the release landing on the field and trip the
    // open-on-mouse-up handler below. On mouse-up the button is still present, so
    // that handler's `e.target != m_ClearBtn` guard correctly suppresses the open.
    m_ClearBtn->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (e.Button == 0)
        {
            OnClearRequested();
            e.Stop();
        }
    });

    // Open on mouse-up so the release that triggered the picker does not clear
    // focus from the dialog's search field the moment it appears.
    RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (e.Button == 0 && e.Target != m_ClearBtn)
        {
            OpenReferenceDialog();
            e.Stop();
        }
    });
}

void ReferenceFieldBase::OpenReferenceDialog()
{
    if (m_ActiveDialog)
        return;

    UIManager* mgr = GetOwnerManager();
    if (!mgr)
        return;
    UIElement* root = mgr->GetRootElement();
    if (!root)
        return;

    std::unique_ptr<ISearchProvider> provider = CreateProvider();
    if (!provider)
        return;
    m_SearchProvider = std::move(provider);

    auto dialog = std::make_unique<SearchDialog>();
    dialog->SetProvider(m_SearchProvider.get());
    dialog->SetInitialSelection(CurrentResultId());

    SearchDialog* dialogPtr = dialog.get();
    m_ActiveDialog = dialogPtr;
    AddClass("open");

    dialog->SetOnResult([this, dialogPtr, root](const SearchResultItem& item) {
        OnResultSelected(item);
        m_ActiveDialog = nullptr;
        RemoveClass("open");
        m_SearchProvider.reset();
        root->RemoveChild(dialogPtr);
    });
    dialog->SetOnCancel([this, dialogPtr, root]() {
        m_ActiveDialog = nullptr;
        RemoveClass("open");
        m_SearchProvider.reset();
        root->RemoveChild(dialogPtr);
    });

    root->AddChild(std::move(dialog));
    PositionDialog(*dialogPtr);
    dialogPtr->Show();
}

void ReferenceFieldBase::ShowEmpty()
{
    m_NameLabel->SetText("(None)");
    m_NameLabel->AddClass("empty");
    m_ClearBtn->AddClass("hidden");
}

void ReferenceFieldBase::ShowNamed(const std::string& name)
{
    m_NameLabel->SetText(name);
    m_NameLabel->RemoveClass("empty");
    if (m_Clearable)
        m_ClearBtn->RemoveClass("hidden");
}

void ReferenceFieldBase::SetClearable(bool clearable)
{
    m_Clearable = clearable;
    // .hidden is display:none, so the button leaves layout entirely and cannot be
    // hit-tested — the mouse-up handler it carries needs no further guard.
    if (!clearable && m_ClearBtn)
        m_ClearBtn->AddClass("hidden");
}

bool ReferenceFieldBase::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    if (!ContainsPoint(x, y))
        return false;
    out.TargetId = 0;
    out.Location = UI::Interaction::DropLocation::OnItem;
    return true;
}

void ReferenceFieldBase::SetDropPreview(const UI::Interaction::DropPreviewState& state)
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

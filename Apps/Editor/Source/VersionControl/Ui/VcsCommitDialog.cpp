#include "VersionControl/Ui/VcsCommitDialog.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"

namespace GameEngine::Editor {

VcsCommitDialog::VcsCommitDialog() = default;
VcsCommitDialog::~VcsCommitDialog() = default;

void VcsCommitDialog::Show(GameEngine::UIManager* uiManager,
                           const std::string& defaultMessage,
                           std::function<void(const std::string&)> onCommit,
                           std::function<void()> onCancel)
{
    if (!uiManager)
    {
        Logger::Log::Error("VcsCommitDialog: UIManager is null");
        return;
    }

    Hide(); // re-Show replaces the dialog instead of stacking a second one

    m_UIManager = uiManager;
    m_OnCommit = std::move(onCommit);
    m_OnCancel = std::move(onCancel);
    m_IsVisible = true;

    // Create dialog root as a full-screen backdrop that centers a bounded
    // window (the editor's modal idiom; see VcsDialog.css). Without the
    // stylesheet the root would flow as an unstyled full-width strip.
    auto dialogRoot = std::make_unique<GameEngine::UIElement>("div");
    m_DialogRoot = dialogRoot.get();
    m_DialogRoot->SetId("vcs-commit-dialog");
    m_DialogRoot->AddClass("vcs-modal");
    m_DialogRoot->AddClass("vcs-commit-dialog");
    m_DialogRoot->RequestSubtreeStyleAssetPath("UI/controls/VcsDialog.css", "editor");
    m_DialogRoot->SetFocusable(true);
    m_DialogRoot->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            if (m_OnCancel)
                m_OnCancel();
            Hide();
        }
    });

    // Bounded window that holds the dialog content.
    auto window = std::make_unique<GameEngine::UIElement>("div");
    auto* windowPtr = window.get();
    windowPtr->AddClass("modal-window");
    windowPtr->AddClass("vcs-modal-window");

    // Title
    auto title = std::make_unique<GameEngine::Label>();
    title->SetText("Commit Changes");
    title->AddClass("vcs-modal-title");
    windowPtr->AddChild(std::move(title));

    // Message label
    auto msgLabel = std::make_unique<GameEngine::Label>();
    msgLabel->SetText("Commit Message:");
    msgLabel->AddClass("vcs-modal-label");
    windowPtr->AddChild(std::move(msgLabel));

    // Message text field
    auto textField = std::make_unique<GameEngine::TextField>();
    textField->SetId("commit-message-field");
    textField->SetValue(defaultMessage);
    windowPtr->AddChild(std::move(textField));

    // Buttons container
    auto buttonContainer = std::make_unique<GameEngine::UIElement>("div");
    buttonContainer->AddClass("vcs-modal-buttons");

    // Commit button
    auto commitBtn = std::make_unique<GameEngine::Button>();
    commitBtn->SetText("Commit");
    commitBtn->SetId("commit-button");
    commitBtn->RegisterEventHandler(kEventButtonClick, [this, uiManager](UIEvent&)
    {
        if (auto* root = uiManager->GetRootElement())
        {
            if (auto* field = root->FindById("commit-message-field"))
            {
                if (auto* tf = dynamic_cast<GameEngine::TextField*>(field))
                {
                    std::string message = tf->GetValue();
                    if (!message.empty() && m_OnCommit)
                    {
                        m_OnCommit(message);
                    }
                }
            }
        }
        Hide();
    });
    buttonContainer->AddChild(std::move(commitBtn));

    // Cancel button
    auto cancelBtn = std::make_unique<GameEngine::Button>();
    cancelBtn->SetText("Cancel");
    cancelBtn->SetId("cancel-button");
    cancelBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
    {
        if (m_OnCancel)
        {
            m_OnCancel();
        }
        Hide();
    });
    buttonContainer->AddChild(std::move(cancelBtn));

    windowPtr->AddChild(std::move(buttonContainer));
    m_DialogRoot->AddChild(std::move(window));

    // Add to UI root (the root owns the element; m_DialogRoot stays as the
    // non-owning handle for Hide).
    if (auto* root = uiManager->GetRootElement())
    {
        root->AddChild(std::move(dialogRoot));
        uiManager->PostToUI([this, uiManager]()
        {
            if (m_DialogRoot && m_UIManager == uiManager)
            {
                if (auto* field = dynamic_cast<GameEngine::TextField*>(
                        m_DialogRoot->FindById("commit-message-field")))
                {
                    uiManager->FocusElement(field);
                    field->SelectAll();
                }
            }
        });
    }
    else
    {
        m_DialogRoot = nullptr;
    }
}

void VcsCommitDialog::Hide()
{
    m_IsVisible = false;
    if (m_DialogRoot && m_UIManager)
    {
        if (auto* root = m_UIManager->GetRootElement())
        {
            root->RemoveChild(m_DialogRoot);
        }
    }
    m_DialogRoot = nullptr;
    m_UIManager = nullptr;
}

} // namespace GameEngine::Editor

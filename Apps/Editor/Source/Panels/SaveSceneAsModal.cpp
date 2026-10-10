#include "Panels/SaveSceneAsModal.h"

#include "Platform/Capabilities.h"
#include "Platform/Shell.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "Input/KeyCodes.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace GameEngine
{

namespace
{
static std::string SanitizeFileStem(std::string s)
{
    // Very small sanitizer: keep alnum, '_' '-' and space; replace others with '_'.
    for (auto& ch : s)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '_' || ch == '-' || ch == ' ')
            continue;
        ch = '_';
    }
    // trim spaces
    while (!s.empty() && s.front() == ' ')
        s.erase(s.begin());
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    if (s.empty())
        s = "NewScene";
    return s;
}
} // namespace

SaveSceneAsModal::SaveSceneAsModal()
{
    AddClass("modal-overlay");
    SetOverlayLayer(OverlayLayer::BlockingDialog);

    auto backdrop = std::make_unique<UIElement>();
    backdrop->AddClass("modal-backdrop");

    auto window = std::make_unique<UIElement>();
    window->AddClass("modal-window");
    window->AddClass("modal-window-shadow");
    window->AddClass("modal-window-560");

    // Header
    {
        auto header = std::make_unique<UIElement>();
        header->AddClass("modal-header");
        auto title = std::make_unique<Label>();
        title->SetText("Save Scene As");
        title->AddClass("modal-title");
        header->AddChild(std::move(title));
        window->AddChild(std::move(header));
    }

    // Content
    {
        auto content = std::make_unique<UIElement>();
        content->AddClass("modal-content");
        content->AddClass("modal-content-column");

        {
            auto row = std::make_unique<UIElement>();
            row->AddClass("modal-field-row");
            auto label = std::make_unique<Label>();
            label->SetText("Scene name");
            label->AddClass("modal-field-label");
            row->AddChild(std::move(label));

            auto field = std::make_unique<TextField>();
            m_NameField = field.get();
            field->SetValue("NewScene");
            field->AddClass("modal-text-field");
            field->SetOnCommit([this]() { OnSaveClicked(); });
            row->AddChild(std::move(field));
            content->AddChild(std::move(row));
        }

        {
            auto row = std::make_unique<UIElement>();
            row->AddClass("modal-field-row");
            auto label = std::make_unique<Label>();
            label->SetText("Folder");
            label->AddClass("modal-field-label");
            row->AddChild(std::move(label));

            auto dirRow = std::make_unique<UIElement>();
            dirRow->AddClass("save-scene-as-directory-row");

            auto browse = std::make_unique<Button>();
            browse->SetText("Browse...");
            browse->AddClass("secondary");
            browse->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnBrowseClicked(); });

            auto dirLabel = std::make_unique<Label>();
            m_DirLabel = dirLabel.get();
            dirLabel->SetText("(none)");
            dirLabel->AddClass("save-scene-as-directory-label");

            dirRow->AddChild(std::move(browse));
            dirRow->AddChild(std::move(dirLabel));
            row->AddChild(std::move(dirRow));
            content->AddChild(std::move(row));

            // Web-only project-folder chooser. Browse drills through the open
            // project's own subfolders here instead of the OS importer; the rows
            // are empty (and the block collapses) until Browse is clicked.
            auto folderList = std::make_unique<UIElement>();
            m_FolderList = folderList.get();
            folderList->AddClass("save-scene-as-folder-list");
            content->AddChild(std::move(folderList));
        }

        {
            auto warn = std::make_unique<Label>();
            m_OverwriteWarningLabel = warn.get();
            warn->SetText("");
            warn->AddClass("save-scene-as-warning");
            content->AddChild(std::move(warn));
        }

        window->AddChild(std::move(content));
    }

    // Footer buttons
    {
        auto footer = std::make_unique<UIElement>();
        footer->AddClass("modal-footer");

        auto cancel = std::make_unique<Button>();
        cancel->SetText("Cancel");
        cancel->AddClass("secondary");
        cancel->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnCancelClicked(); });

        auto save = std::make_unique<Button>();
        m_SaveButton = save.get();
        save->SetText("Save");
        save->AddClass("primary");
        save->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { OnSaveClicked(); });

        footer->AddChild(std::move(cancel));
        footer->AddChild(std::move(save));
        window->AddChild(std::move(footer));
    }

    backdrop->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if (!m_Visible)
            return;
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            e.Handled = true;
            OnSaveClicked();
        }
        else if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            OnCancelClicked();
        }
    });

    backdrop->AddChild(std::move(window));
    AddChild(std::move(backdrop));
}

void SaveSceneAsModal::Show(const std::filesystem::path& initialDirectory)
{
    m_Visible = true;
    ClearOverwritePrompt();
    m_SelectedDir = initialDirectory;
    m_ProjectRoot = initialDirectory; // Confine the web folder chooser to this subtree.
    if (m_FolderList)
        m_FolderList->RemoveAllChildren();
    if (m_DirLabel)
        m_DirLabel->SetText(m_SelectedDir.empty() ? "(none)" : m_SelectedDir.string());

    AddClass("visible");

    if (m_NameField)
    {
        // Defer focus until after the next UI tick: Show() flips display from None to Block,
        // but the element isn't fully mounted/laid out until the dispatcher drains, so an
        // immediate FocusElement can no-op on a freshly visible subtree.
        if (auto* mgr = GetOwnerManager())
        {
            TextField* field = m_NameField;
            mgr->PostToUI([mgr, field]()
            {
                mgr->FocusElement(field);
                field->SelectAll();
            });
        }
        else
        {
            m_NameField->SelectAll();
        }
    }
}

void SaveSceneAsModal::Hide()
{
    m_Visible = false;
    ClearOverwritePrompt();
    if (m_FolderList)
        m_FolderList->RemoveAllChildren();
    RemoveClass("visible");
}

void SaveSceneAsModal::OnBrowseClicked()
{
    if (Platform::ProjectStorageIsSandboxed())
    {
        // An OS folder dialog cannot name a location inside the project here (it
        // would import an arbitrary folder instead). Toggle the in-project
        // chooser: a second click closes it.
        if (m_FolderList && !m_FolderList->GetChildren().empty())
        {
            m_FolderList->RemoveAllChildren();
            return;
        }
        PopulateFolderList();
        return;
    }
    const std::filesystem::path picked = Platform::SelectFolder(m_SelectedDir);
    if (picked.empty())
        return;
    m_SelectedDir = picked;
    if (m_DirLabel)
        m_DirLabel->SetText(m_SelectedDir.string());
}

void SaveSceneAsModal::PopulateFolderList()
{
    if (!m_FolderList)
        return;
    m_FolderList->RemoveAllChildren();

    auto addRow = [this](const std::string& text, const std::filesystem::path& target) {
        auto btn = std::make_unique<Button>();
        btn->SetText(text);
        btn->AddClass("save-scene-as-folder-row");
        btn->AddClass("secondary");
        btn->RegisterEventHandler(kEventButtonClick, [this, target](UIEvent&) {
            m_SelectedDir = target;
            if (m_DirLabel)
                m_DirLabel->SetText(m_SelectedDir.string());
            PopulateFolderList();
        });
        m_FolderList->AddChild(std::move(btn));
    };

    // Up one level, bounded at the project root so the destination cannot escape it.
    if (!m_ProjectRoot.empty() && m_SelectedDir != m_ProjectRoot)
        addRow("[ .. ]", m_SelectedDir.parent_path());

    // Immediate subfolders only: one level is the single read the UI thread may
    // make on every platform (see FileSystem::ListDirectories).
    std::vector<std::filesystem::path> dirs;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(m_SelectedDir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code de;
        if (!it->is_directory(de))
            continue;
        const std::string name = it->path().filename().string();
        if (!name.empty() && name.front() != '.')
            dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs)
        addRow(d.filename().string(), d);
}

void SaveSceneAsModal::ClearOverwritePrompt()
{
    m_PendingOverwrite = false;
    m_PendingOverwritePath.clear();
    if (m_OverwriteWarningLabel)
        m_OverwriteWarningLabel->RemoveClass("visible");
    if (m_SaveButton)
        m_SaveButton->SetText("Save");
}

void SaveSceneAsModal::CommitSave(const std::filesystem::path& outPath)
{
    Hide();
    if (m_OnSave)
        m_OnSave(outPath);
}

void SaveSceneAsModal::OnSaveClicked()
{
    if (!m_NameField)
        return;
    std::string stem = SanitizeFileStem(m_NameField->GetValue());
    if (m_SelectedDir.empty())
        return;

    const std::filesystem::path outPath = (m_SelectedDir / (stem + ".scene")).lexically_normal();

    if (m_PendingOverwrite)
    {
        if (outPath != m_PendingOverwritePath)
            ClearOverwritePrompt();
        else
        {
            CommitSave(outPath);
            return;
        }
    }

    const bool fileExists =
        std::filesystem::exists(outPath) && std::filesystem::is_regular_file(outPath);
    if (fileExists)
    {
        m_PendingOverwrite = true;
        m_PendingOverwritePath = outPath;
        if (m_OverwriteWarningLabel)
        {
            m_OverwriteWarningLabel->SetText(
                "A scene file already exists at this path. Replace the existing file?");
            m_OverwriteWarningLabel->AddClass("visible");
        }
        if (m_SaveButton)
            m_SaveButton->SetText("Overwrite");
        return;
    }

    CommitSave(outPath);
}

void SaveSceneAsModal::OnCancelClicked()
{
    if (m_PendingOverwrite)
    {
        ClearOverwritePrompt();
        return;
    }
    Hide();
    if (m_OnCancel)
        m_OnCancel();
}

} // namespace GameEngine

#include "Panels/ShaderErrorsPanel.h"

#include "Core/Engine.h"
#include "Editor/EditorPaths.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Engine/Rendering/RenderServices.h"
#include "Input/KeyCodes.h"
#include "Platform/Clipboard.h"
#include "Platform/ContextMenu.h"
#include "UI/EditorIcons.h"
#include "UI/Controls/Label.h"
#include "UI/UIEvents.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace GameEngine {

namespace {

constexpr uint32_t kCopyErrorCommandId = 1;
constexpr uint32_t kCopyAllErrorsCommandId = 2;
constexpr uint32_t kOpenInScriptEditorCommandId = 3;

} // namespace

class ShaderErrorsPanel::RowsProvider final : public ListChangeTrackingProvider
{
public:
    explicit RowsProvider(const std::vector<ShaderErrorRow>& rows) : m_Rows(rows) {}

    int GetItemCount() const override { return static_cast<int>(m_Rows.size()); }
    ListId GetItemId(int index) const override
    {
        return index >= 0 && index < GetItemCount() ? static_cast<ListId>(index + 1) : 0;
    }
    float GetItemHeight(int) const override { return 46.0f; }

    void MarkReset() { MarkAllChanged(); }

private:
    const std::vector<ShaderErrorRow>& m_Rows;
};

ShaderErrorsPanel::ShaderErrorsPanel()
    : DockPanel("Shader Errors")
{
    auto container = std::make_unique<UIElement>();
    container->AddClass("script-errors-split"); // shares ScriptErrorsPanel.css

    m_RowsProvider = std::make_unique<RowsProvider>(m_Rows);
    auto list = std::make_unique<ListView>();
    list->AddClass("script-errors-list-scroll");
    list->AddClass("script-errors-list");
    list->SetDataProvider(m_RowsProvider.get());
    list->SetFocusable(true);
    m_RowListView = list.get();

    list->SetItemFactory([](ListId, IListDataProvider*) {
        auto row = std::make_unique<UIElement>();
        row->AddClass("script-error-entry");
        row->SetTooltip("Double-click to open the shader at this line");

        auto headerRow = std::make_unique<UIElement>();
        headerRow->AddClass("script-error-header-row");
        auto errorTypeLabel = std::make_unique<Label>();
        errorTypeLabel->AddClass("error-type");
        headerRow->AddChild(std::move(errorTypeLabel));
        auto errorLocLabel = std::make_unique<Label>();
        errorLocLabel->AddClass("error-location");
        headerRow->AddChild(std::move(errorLocLabel));
        row->AddChild(std::move(headerRow));

        auto detailRow = std::make_unique<UIElement>();
        detailRow->AddClass("script-error-detail-row");
        auto materialLabel = std::make_unique<Label>();
        materialLabel->AddClass("error-code");
        detailRow->AddChild(std::move(materialLabel));
        auto errorMessage = std::make_unique<Label>();
        errorMessage->AddClass("error-message");
        detailRow->AddChild(std::move(errorMessage));
        row->AddChild(std::move(detailRow));
        return row;
    });

    list->SetItemBinder([this](UIElement* element, ListId, int index, IListDataProvider*) {
        if (!element || index < 0 || index >= static_cast<int>(m_Rows.size()))
            return;
        const ShaderErrorRow& row = m_Rows[static_cast<size_t>(index)];

        const auto& rowChildren = element->GetChildren();
        if (rowChildren.size() < 2)
            return;
        const auto& headerChildren = rowChildren[0]->GetChildren();
        const auto& detailChildren = rowChildren[1]->GetChildren();
        if (headerChildren.size() < 2 || detailChildren.size() < 2)
            return;
        auto* errorTypeLabel = dynamic_cast<Label*>(headerChildren[0].get());
        auto* errorLocLabel = dynamic_cast<Label*>(headerChildren[1].get());
        auto* materialLabel = dynamic_cast<Label*>(detailChildren[0].get());
        auto* errorMessage = dynamic_cast<Label*>(detailChildren[1].get());
        if (!errorTypeLabel || !errorLocLabel || !materialLabel || !errorMessage)
            return;

        errorTypeLabel->AddClass("error");
        errorTypeLabel->SetText("Error");

        std::string location;
        if (!row.DisplayPath.empty())
        {
            location = row.DisplayPath;
            if (row.Line > 0)
            {
                location += ':';
                location += std::to_string(row.Line);
            }
        }
        errorLocLabel->SetText(location);
        materialLabel->SetText(row.MaterialName);
        errorMessage->SetText(row.Message);
    });

    list->SetOnSelectionChanged([this](ListId id) {
        SelectEntry(id > 0 ? static_cast<int>(id - 1) : -1);
    });
    list->SetOnItemActivated([this](ListId id) {
        OpenEntry(id > 0 ? static_cast<int>(id - 1) : -1);
    });
    list->SetOnContextMenu([this](ListId id, float x, float y) {
        ShowContextMenu(id > 0 ? static_cast<int>(id - 1) : -1, x, y);
    });
    list->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (e.Key != Input::kKeyCode_C || !Input::IsPrimaryShortcutModifier(e.Mods))
            return;
        const int selectedIndex = m_RowListView ? m_RowListView->GetSelectedIndex() : m_SelectedIndex;
        if (CopyErrorToClipboard(selectedIndex))
            e.Stop();
    });

    container->AddChild(std::move(list));

    // Detail view for the selected row. The list column is too narrow to read a compiler
    // diagnostic in, so the full untruncated message lives here. There is deliberately no
    // source preview: double-clicking a row opens the real Script Editor at the failing
    // line, which a pane this size cannot do as well.
    auto detail = std::make_unique<UIElement>();
    detail->AddClass("script-errors-detail");

    auto detailMessage = std::make_unique<Label>();
    detailMessage->AddClass("script-errors-detail-message");
    m_DetailMessage = detailMessage.get();
    detail->AddChild(std::move(detailMessage));

    container->AddChild(std::move(detail));

    AddChild(std::move(container));

    ShowDetail(nullptr);
}

ShaderErrorsPanel::~ShaderErrorsPanel() = default;

void ShaderErrorsPanel::Update()
{
    auto* rs = EngineCore::GetInstance().GetRenderServices();
    if (!rs)
        return;
    auto& log = rs->Materials().ShaderErrors();
    const uint64_t version = log.Version();
    if (version == m_SeenVersion)
        return;
    m_SeenVersion = version;

    RebuildRows();

    // Reveal on new failures — a broken save should be seen, not discovered.
    // Repeat failures re-reveal too: the author just hit save again.
    if (!m_Rows.empty() && m_OnRequestReveal)
        m_OnRequestReveal();
}

void ShaderErrorsPanel::RebuildRows()
{
    m_Rows.clear();
    m_SelectedIndex = -1;

    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        const Editor::EditorGlobalPaths globalPaths = Editor::GetEditorGlobalPaths();
        m_Rows = BuildShaderErrorRows(
            rs->Materials().ShaderErrors().Snapshot(),
            {Editor::GetCurrentEditorProjectPaths().projectRoot,
             {globalPaths.installAssetsRoot, globalPaths.userAssetsRoot}});
    }

    if (m_RowsProvider)
        m_RowsProvider->MarkReset();
    if (m_RowListView)
        m_RowListView->RefreshFromProvider();

    if (m_Rows.empty())
    {
        ShowDetail(nullptr);
        return;
    }

    // Select the first row so the detail view describes the failure the author just caused,
    // rather than showing an empty pane until they think to click something.
    if (m_RowListView)
        m_RowListView->SetSelectedIndex(0);
    else
        SelectEntry(0);
}

void ShaderErrorsPanel::SelectEntry(int index)
{
    m_SelectedIndex = index;
    if (index < 0 || index >= static_cast<int>(m_Rows.size()))
    {
        ShowDetail(nullptr);
        return;
    }

    ShowDetail(&m_Rows[static_cast<size_t>(index)]);
}

void ShaderErrorsPanel::ShowDetail(const ShaderErrorRow* row)
{
    if (!m_DetailMessage)
        return;

    if (!row)
    {
        m_DetailMessage->SetText("No shader compile errors.");
        return;
    }

    std::string text;
    if (!row->DisplayPath.empty())
    {
        text = row->DisplayPath;
        if (row->Line > 0)
        {
            text += ':';
            text += std::to_string(row->Line);
        }
    }
    if (!row->MaterialName.empty())
    {
        if (!text.empty())
            text += "  —  ";
        text += row->MaterialName;
    }
    if (!row->Message.empty())
    {
        if (!text.empty())
            text += '\n';
        text += row->Message;
    }
    m_DetailMessage->SetText(text);
}

void ShaderErrorsPanel::OpenEntry(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Rows.size()))
        return;
    const ShaderErrorRow& row = m_Rows[static_cast<size_t>(index)];
    if (m_OnOpenInEditor && !row.File.empty())
        m_OnOpenInEditor(std::filesystem::path(row.File), row.Line, 0);
}

void ShaderErrorsPanel::OpenEntryInScriptEditor(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Rows.size()))
        return;
    const ShaderErrorRow& row = m_Rows[static_cast<size_t>(index)];
    if (m_OnOpenInScriptEditor && !row.File.empty())
        m_OnOpenInScriptEditor(std::filesystem::path(row.File), row.Line, 0);
}

void ShaderErrorsPanel::ShowContextMenu(int index, float x, float y)
{
    if (index < 0 || index >= static_cast<int>(m_Rows.size()) || !m_Window)
        return;

    m_ContextMenuIndex = index;
    if (!m_ContextMenu)
        m_ContextMenu = CreateContextMenu();
    if (!m_ContextMenu)
        return;

    m_ContextMenu->Clear();
    m_ContextMenu->SetCommandHandler([this](uint32_t commandId) {
        if (commandId == kCopyErrorCommandId)
            (void)CopyErrorToClipboard(m_ContextMenuIndex);
        else if (commandId == kCopyAllErrorsCommandId)
            (void)CopyAllErrorsToClipboard();
        else if (commandId == kOpenInScriptEditorCommandId)
            OpenEntryInScriptEditor(m_ContextMenuIndex);
    });

    ContextMenuBuilder builder;
    builder.AddItem("Open in Script Editor", kOpenInScriptEditorCommandId, MenuItemFlag_None, 0,
                    EditorIcons::kFolderOpen);
    builder.AddItem("Copy Error", kCopyErrorCommandId, MenuItemFlag_None, 0, EditorIcons::kCopy);
    builder.AddItem("Copy All Errors", kCopyAllErrorsCommandId, MenuItemFlag_None, 0, EditorIcons::kCopy);
    builder.Build(m_ContextMenu.get());
    m_ContextMenu->Show(m_Window, static_cast<int>(x), static_cast<int>(y));
}

bool ShaderErrorsPanel::CopyErrorToClipboard(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Rows.size()))
        return false;
    const ShaderErrorRow& row = m_Rows[static_cast<size_t>(index)];
    std::string text = row.MaterialName;
    if (!row.File.empty())
    {
        text += "\n" + row.File;
        if (row.Line > 0)
            text += ":" + std::to_string(row.Line);
    }
    if (!row.Message.empty())
        text += "\n" + row.Message;
    if (text.empty())
        return false;
    Platform::SetClipboardText(text.c_str());
    return true;
}

bool ShaderErrorsPanel::CopyAllErrorsToClipboard() const
{
    if (m_Rows.empty())
        return false;
    std::string text;
    for (const ShaderErrorRow& row : m_Rows)
    {
        if (!text.empty())
            text += "\n\n";
        text += row.MaterialName;
        if (!row.File.empty())
        {
            text += "\n" + row.File;
            if (row.Line > 0)
                text += ":" + std::to_string(row.Line);
        }
        if (!row.Message.empty())
            text += "\n" + row.Message;
    }
    Platform::SetClipboardText(text.c_str());
    return true;
}

} // namespace GameEngine

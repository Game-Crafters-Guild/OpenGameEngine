#include "Panels/ScriptErrorsPanel.h"
#include "AssetCore/SharedFileRead.h"

#include "Core/Engine.h"
#include "Editor/EditorPaths.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Input/KeyCodes.h"
#include "Jobs/CompileServerClient.h"
#include "Panels/ScriptTextArea.h"
#include "Platform/Clipboard.h"
#include "Platform/ContextMenu.h"
#include "Scripting/ScriptManager.h"
#include "UI/EditorIcons.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ListView.h"
#include "UI/Controls/ScrollView.h"
#include "UI/UIEvents.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace GameEngine {

namespace {

constexpr uint32_t kCopyErrorCommandId = 1;
constexpr uint32_t kCopyAllErrorsCommandId = 2;
constexpr uint32_t kOpenInScriptEditorCommandId = 3;

unsigned char ToUChar(char c) { return static_cast<unsigned char>(c); }
bool IsSpace(char c) { return std::isspace(ToUChar(c)) != 0; }
bool IsAlnum(char c) { return std::isalnum(ToUChar(c)) != 0; }

std::string FormatDiagnosticForClipboard(const CompileServerDiagnostic& diag)
{
    std::string text = diag.Severity;
    if (!diag.FileUtf8.empty())
    {
        if (!text.empty())
            text += ' ';
        text += diag.FileUtf8;
        if (diag.Line > 0)
        {
            text += ':';
            text += std::to_string(diag.Line);
            if (diag.Column > 0)
            {
                text += ',';
                text += std::to_string(diag.Column);
            }
        }
    }

    if (!diag.Code.empty() || !diag.MessageUtf8.empty())
    {
        if (!text.empty())
            text += '\n';
        if (!diag.Code.empty())
        {
            text += diag.Code;
            if (!diag.MessageUtf8.empty())
                text += ' ';
        }
        text += diag.MessageUtf8;
    }

    return text;
}

bool IsSameDiagnostic(const CompileServerDiagnostic& a, const CompileServerDiagnostic& b)
{
    return a.FileUtf8 == b.FileUtf8 && a.Line == b.Line && a.Column == b.Column && a.Code == b.Code &&
           a.MessageUtf8 == b.MessageUtf8;
}

} // namespace

class ScriptErrorsPanel::DiagnosticsProvider final : public ListChangeTrackingProvider
{
public:
    explicit DiagnosticsProvider(const std::vector<CompileServerDiagnostic>& diagnostics)
        : m_Diagnostics(diagnostics)
    {
    }

    int GetItemCount() const override { return static_cast<int>(m_Diagnostics.size()); }
    ListId GetItemId(int index) const override { return index >= 0 && index < GetItemCount() ? static_cast<ListId>(index + 1) : 0; }
    float GetItemHeight(int) const override { return 46.0f; }

    void MarkReset() { MarkAllChanged(); }

private:
    const std::vector<CompileServerDiagnostic>& m_Diagnostics;
};

ScriptErrorsPanel::ScriptErrorsPanel()
    : DockPanel("Script Errors")
{
    auto container = std::make_unique<UIElement>();
    container->AddClass("script-errors-split");

    // Error list (left)
    m_DiagnosticsProvider = std::make_unique<DiagnosticsProvider>(m_Diagnostics);
    auto list = std::make_unique<ListView>();
    list->AddClass("script-errors-list-scroll");
    list->AddClass("script-errors-list");
    list->SetDataProvider(m_DiagnosticsProvider.get());
    list->SetFocusable(true);
    m_DiagnosticListView = list.get();

    list->SetItemFactory([](ListId, IListDataProvider*) {
        auto row = std::make_unique<UIElement>();
        row->AddClass("script-error-entry");
        row->SetTooltip("Double-click to open in editor");

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

        auto errorCode = std::make_unique<Label>();
        errorCode->AddClass("error-code");
        detailRow->AddChild(std::move(errorCode));

        auto errorMessage = std::make_unique<Label>();
        errorMessage->AddClass("error-message");
        detailRow->AddChild(std::move(errorMessage));

        row->AddChild(std::move(detailRow));
        return row;
    });

    list->SetItemBinder([this](UIElement* element, ListId, int index, IListDataProvider*) {
        if (!element || index < 0 || index >= static_cast<int>(m_Diagnostics.size()))
            return;

        const auto& diag = m_Diagnostics[static_cast<size_t>(index)];
        const bool isError = diag.Severity != "Warning";

        const auto& rowChildren = element->GetChildren();
        if (rowChildren.size() < 2)
            return;

        auto* headerRow = rowChildren[0].get();
        auto* detailRow = rowChildren[1].get();
        const auto& headerChildren = headerRow->GetChildren();
        const auto& detailChildren = detailRow->GetChildren();
        if (headerChildren.size() < 2 || detailChildren.size() < 2)
            return;

        auto* errorTypeLabel = dynamic_cast<Label*>(headerChildren[0].get());
        auto* errorLocLabel = dynamic_cast<Label*>(headerChildren[1].get());
        auto* errorCode = dynamic_cast<Label*>(detailChildren[0].get());
        auto* errorMessage = dynamic_cast<Label*>(detailChildren[1].get());
        if (!errorTypeLabel || !errorLocLabel || !errorCode || !errorMessage)
            return;

        errorTypeLabel->RemoveClass("error");
        errorTypeLabel->RemoveClass("warning");
        errorTypeLabel->AddClass(isError ? "error" : "warning");
        errorTypeLabel->SetText(diag.Severity);

        std::string errorLocation;
        std::string fileDisplay = diag.FileUtf8;
        if (!fileDisplay.empty())
        {
            const auto projectRoot = Editor::GetCurrentEditorProjectPaths().projectRoot;
            if (!projectRoot.empty())
            {
                auto filePath = std::filesystem::path(diag.FileUtf8);
                std::error_code ec;
                auto rel = std::filesystem::relative(filePath, projectRoot, ec);
                if (!ec && !rel.empty() && rel.native().find(std::filesystem::path("..").native()) == std::string::npos)
                    fileDisplay = rel.generic_string();
            }

            errorLocation += fileDisplay;
            if (diag.Line > 0)
            {
                errorLocation += ':';
                errorLocation += std::to_string(diag.Line);
                if (diag.Column > 0)
                {
                    errorLocation += ',';
                    errorLocation += std::to_string(diag.Column);
                }
            }
        }

        errorLocLabel->SetText(errorLocation);
        errorCode->SetText(diag.Code);
        errorMessage->SetText(diag.MessageUtf8);
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

        const int selectedIndex = m_DiagnosticListView ? m_DiagnosticListView->GetSelectedIndex() : m_SelectedIndex;
        if (CopyErrorToClipboard(selectedIndex))
            e.Stop();
    });

    container->AddChild(std::move(list));

    // Script preview (right)
    auto previewScroll = std::make_unique<ScrollView>();
    previewScroll->AddClass("script-errors-preview");
    m_PreviewScrollView = previewScroll.get();

    auto textArea = std::make_unique<ScriptTextArea>();
    textArea->AddClass("script-editor-textarea");
    textArea->SetShowLineNumbers(true);
    textArea->SetReadOnly(true);
    m_PreviewTextArea = textArea.get();

    previewScroll->SetOnScrollChanged([this](float, float) {
        if (m_PreviewTextArea)
        {
            m_PreviewTextArea->SyncFoldMarkerPositionsToScroll();
            m_PreviewTextArea->MarkDirty(VisualDirty);
        }
    });
    previewScroll->RegisterEventHandler(kEventScroll, [this](UIEvent&) {
        if (m_PreviewTextArea)
        {
            m_PreviewTextArea->SyncFoldMarkerPositionsToScroll();
            m_PreviewTextArea->MarkDirty(VisualDirty);
        }
    });

    previewScroll->AddContent(std::move(textArea));
    container->AddChild(std::move(previewScroll));

    AddChild(std::move(container));
    SetupObservers();
}

ScriptErrorsPanel::~ScriptErrorsPanel()
{
    TeardownObservers();
}

void ScriptErrorsPanel::Update()
{
    RetainCompiledAssemblies();

    std::vector<CompileServerDiagnostic> rows;
    if (!m_DiagnosticsByAssembly.TakeRowsIfChanged(rows))
        return;

    int keptSelection = -1;
    if (m_SelectedIndex >= 0 && m_SelectedIndex < static_cast<int>(m_Diagnostics.size()))
    {
        const CompileServerDiagnostic& selected = m_Diagnostics[static_cast<size_t>(m_SelectedIndex)];
        const auto it = std::find_if(rows.begin(), rows.end(), [&selected](const CompileServerDiagnostic& row) {
            return IsSameDiagnostic(row, selected);
        });
        if (it != rows.end())
            keptSelection = static_cast<int>(it - rows.begin());
    }
    m_Diagnostics = std::move(rows);
    ShowRows(keptSelection);
}

void ScriptErrorsPanel::RetainCompiledAssemblies()
{
    const ScriptManager& scriptManager = EngineCore::GetInstance().GetScriptManager();
    const uint64_t revision = scriptManager.GetPackageCodeModulesRevision();
    if (revision == m_SeenPackageCodeModulesRevision)
        return;
    m_SeenPackageCodeModulesRevision = revision;
    m_DiagnosticsByAssembly.SetCompiledAssemblies(scriptManager.GetCompiledAssemblyNames());
}

void ScriptErrorsPanel::SetupObservers()
{
    m_DiagnosticsObserverId = CompileServerClient::RegisterDiagnosticsBatchObserver(
        [this](const std::string& assemblyName, const std::vector<CompileServerDiagnostic>& diagnostics) {
            m_DiagnosticsByAssembly.OnDiagnostics(assemblyName, diagnostics);
        });

    m_CompileStartedObserverId = CompileServerClient::RegisterCompileStartedObserver(
        [this](const std::string& assemblyName) { m_DiagnosticsByAssembly.OnCompileStarted(assemblyName); });
}

void ScriptErrorsPanel::TeardownObservers()
{
    if (m_DiagnosticsObserverId != 0)
    {
        CompileServerClient::UnregisterDiagnosticsBatchObserver(m_DiagnosticsObserverId);
        m_DiagnosticsObserverId = 0;
    }
    if (m_CompileStartedObserverId != 0)
    {
        CompileServerClient::UnregisterCompileStartedObserver(m_CompileStartedObserverId);
        m_CompileStartedObserverId = 0;
    }
}

void ScriptErrorsPanel::ShowRows(int keptSelection)
{
    if (m_DiagnosticsProvider)
        m_DiagnosticsProvider->MarkReset();
    if (m_DiagnosticListView)
        m_DiagnosticListView->RefreshFromProvider();

    if (keptSelection >= 0)
    {
        m_SelectedIndex = keptSelection;
        if (m_DiagnosticListView)
            m_DiagnosticListView->SetSelectedIndex(keptSelection, /*scrollIntoView=*/false);
        return;
    }
    m_SelectedIndex = -1;
    m_PreviewFilePath.clear();
    if (m_PreviewTextArea)
        m_PreviewTextArea->SetValue("");
}

void ScriptErrorsPanel::SelectEntry(int index)
{
    m_SelectedIndex = index;

    if (index >= 0 && index < static_cast<int>(m_Diagnostics.size()))
    {
        const auto& diag = m_Diagnostics[index];
        LoadPreview(diag);
    }
}

void ScriptErrorsPanel::OpenEntry(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Diagnostics.size()))
        return;
    const auto& diag = m_Diagnostics[index];
    if (m_OnOpenInEditor && !diag.FileUtf8.empty())
        m_OnOpenInEditor(std::filesystem::path(diag.FileUtf8), diag.Line, diag.Column);
}

void ScriptErrorsPanel::ShowContextMenu(int index, float x, float y)
{
    if (index < 0 || index >= static_cast<int>(m_Diagnostics.size()) || !m_Window)
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

bool ScriptErrorsPanel::CopyErrorToClipboard(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Diagnostics.size()))
        return false;

    const auto text = FormatDiagnosticForClipboard(m_Diagnostics[static_cast<size_t>(index)]);
    if (!text.empty())
    {
        Platform::SetClipboardText(text.c_str());
        return true;
    }
    return false;
}

void ScriptErrorsPanel::OpenEntryInScriptEditor(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Diagnostics.size()))
        return;
    const auto& diag = m_Diagnostics[index];
    if (!diag.FileUtf8.empty() && m_OnOpenInScriptEditor)
        m_OnOpenInScriptEditor(std::filesystem::path(diag.FileUtf8), diag.Line, diag.Column);
}

bool ScriptErrorsPanel::CopyAllErrorsToClipboard() const
{
    if (m_Diagnostics.empty())
        return false;

    std::string text;
    for (const auto& diag : m_Diagnostics)
    {
        const auto formatted = FormatDiagnosticForClipboard(diag);
        if (formatted.empty())
            continue;
        if (!text.empty())
            text += "\n\n";
        text += formatted;
    }

    if (text.empty())
        return false;

    Platform::SetClipboardText(text.c_str());
    return true;
}

void ScriptErrorsPanel::LoadPreview(const CompileServerDiagnostic& diag)
{
    if (!m_PreviewTextArea || diag.FileUtf8.empty())
        return;

    std::filesystem::path filePath(diag.FileUtf8);
    if (filePath != m_PreviewFilePath)
    {
        GameEngine::String content;
        if (!GameEngine::ReadFileTextShared(filePath, content))
            return;
        m_PreviewTextArea->SetValue(content);
        m_PreviewFilePath = filePath;
    }

    if (diag.Line > 0)
        ScrollPreviewToLine(diag.Line, diag.Column);
}

void ScriptErrorsPanel::ScrollPreviewToLine(size_t lineNumber, size_t diagnosticColumn)
{
    if (!m_PreviewTextArea || !m_PreviewScrollView)
        return;

    const std::string& text = m_PreviewTextArea->GetValue();
    size_t currentLine = 1;
    size_t bytePos = 0;
    while (bytePos < text.size() && currentLine < lineNumber)
    {
        if (text[bytePos] == '\n') ++currentLine;
        ++bytePos;
    }

    m_PreviewTextArea->EnsureBytePositionVisible(bytePos);

    const size_t lineEnd = text.find('\n', bytePos);
    const size_t lineEndPos = (lineEnd != std::string::npos) ? lineEnd : text.size();
    m_PreviewTextArea->SetVariableHighlight(bytePos, lineEndPos - bytePos);
    m_PreviewTextArea->ClearDiagnosticHighlight();

    if (diagnosticColumn > 0 && lineEndPos > bytePos)
    {
        size_t diagPos = bytePos + diagnosticColumn - 1;
        if (diagPos >= lineEndPos)
            diagPos = lineEndPos - 1;
        while (diagPos < lineEndPos && IsSpace(text[diagPos]))
            ++diagPos;

        if (diagPos < lineEndPos)
        {
            size_t diagStart = diagPos;
            size_t diagEnd = diagPos + 1;
            auto isTokenChar = [](char c) {
                return IsAlnum(c) || c == '_';
            };

            if (isTokenChar(text[diagPos]))
            {
                while (diagStart > bytePos && isTokenChar(text[diagStart - 1]))
                    --diagStart;
                while (diagEnd < lineEndPos && isTokenChar(text[diagEnd]))
                    ++diagEnd;
            }
            else
            {
                while (diagEnd < lineEndPos && !IsSpace(text[diagEnd]) && !isTokenChar(text[diagEnd]))
                    ++diagEnd;
            }

            if (diagEnd > diagStart)
                m_PreviewTextArea->SetDiagnosticHighlight(diagStart, diagEnd - diagStart);
        }
    }

    const size_t visualLine = m_PreviewTextArea->GetVisualLineForBytePos(bytePos);
    const float lineH = m_PreviewTextArea->GetLineAdvancePx();
    const float padT = m_PreviewTextArea->GetResolvedStyle().Layout.Padding.Top;
    const float lineY = padT + static_cast<float>(visualLine) * lineH;
    const float viewportH = m_PreviewScrollView->GetClipViewport()
                                ? m_PreviewScrollView->GetClipViewport()->GetLayoutHeight()
                                : 300.0f;
    m_PreviewScrollView->SetScrollY(std::max(0.0f, lineY - viewportH / 2.0f));
}

} // namespace GameEngine

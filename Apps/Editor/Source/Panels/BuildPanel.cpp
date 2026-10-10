#include "Panels/BuildPanel.h"
#include "Editor/Registries/BuildExporterRegistry.h"
#include "Editor/Settings/BuildFolderDefaults.h"
#include "Editor/Settings/BuildRenderPipelineSettings.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Core/EngineVersion.h"
#include "Editor/Settings/RenderPipelineSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/PlayerSourcePreparation.h"
#include "Engine/Build/BuildPlatforms.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/Build/PlayerBuildConfig.h"
#include "JobSystem/JobChannel.h"
#include "Logger/Logger.h"
#include "Platform/Shell.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TextField.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"
#include "UI/StyleProperties.h"
#include "Scheduler/Scheduler.h"
#include "Types/StringUtils.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>

namespace GameEngine {

namespace
{
constexpr float kBuildPanelOutputControlHeightPx = 24.0f;
constexpr float kBuildPanelOutputFontPx = 14.0f;
constexpr size_t kBuildPanelColumnCount = 6;
constexpr int kBuildColumnPlatform = 0;
constexpr int kBuildColumnVersion = 1;
constexpr int kBuildColumnEnabled = 2;
constexpr int kBuildColumnProgress = 3;
constexpr int kBuildColumnCompleted = 4;
constexpr int kBuildColumnTime = 5;

static std::string BuildPlatformDisplayName(const std::string& platformName)
{
    return GameEngine::GetBuildPlatformDisplayName(platformName);
}
} // namespace

static std::function<void()> s_OnBuildVersionChanged;

static std::vector<BuildPlatformEntry> MakeDefaultPlatforms()
{
    std::vector<BuildPlatformEntry> platforms;
    for (const GameEngine::BuildPlatformInfo& info : GameEngine::GetBuildPlatforms())
    {
        BuildPlatformEntry entry;
        entry.Name = std::string(info.Name);
        entry.Version = std::string(info.DefaultVersion);
        entry.Enabled = info.EnabledByDefault;
        platforms.push_back(std::move(entry));
    }
    return platforms;
}

BuildPanel::BuildPanel()
    : DockPanel("Build")
{
    m_Platforms = MakeDefaultPlatforms();
    LoadVersionsFromSettings();
    BuildUI();
    s_OnBuildVersionChanged = [this]() { RefreshVersionsFromSettings(); };
}

void BuildPanel::LoadVersionsFromSettings()
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    Editor::SettingsStore prefs = root.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(root);
    std::string err;
    prefs.Load(&err);
    for (BuildPlatformEntry& entry : m_Platforms)
    {
        std::string key = "build.platform." + entry.Name + ".version";
        std::string version;
        if (prefs.TryGetString(key, version) && !version.empty())
            entry.Version = version;
        else
            entry.Version = "1";
    }
}

void BuildPanel::RefreshVersionsFromSettings()
{
    LoadVersionsFromSettings();
    RebuildRows();
}

BuildPanel::~BuildPanel()
{
    s_OnBuildVersionChanged = nullptr;
    // Cancel, then wait: a build waiting for the editor's native module builds
    // (BuildPipeline::WaitForNativeModuleBuilds) waits on main-thread Ticks, and this
    // destructor blocks the main thread, so a plain wait would never return.
    if (m_Pipeline)
        m_Pipeline->RequestCancel();
    if (m_BuildJob.IsValid())
        m_BuildJob.Wait();
}

void NotifyBuildVersionChanged()
{
    if (s_OnBuildVersionChanged)
        s_OnBuildVersionChanged();
}

void BuildPanel::BuildUI()
{
    auto container = std::make_unique<UIElement>();
    container->AddClass("build-panel");
    m_MainContainer = container.get();

    // Scroll area: header (sticky) + rows in same content so columns line up like list view
    auto scroll = std::make_unique<ScrollView>();
    scroll->AddClass("build-panel-scroll");
    m_ScrollView = scroll.get();
    auto scrollContent = std::make_unique<UIElement>();
    scrollContent->AddClass("build-panel-scroll-content");

    m_ColumnWidths = { 84.0f, 48.0f, 50.0f, 78.0f, 58.0f, 48.0f };
    m_ColumnOrder = { 2, 0, 1, 3, 5, 4 };  // default: Enabled, Platform, Version, Progress, Time, Completed

    auto header = std::make_unique<UIElement>();
    header->AddClass("build-panel-header");
    m_Header = header.get();

    m_Header->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) {
        if (m_IsResizingColumn && m_ResizeColumnIndex >= 0 && m_ResizeColumnIndex < (int)m_ColumnWidths.size())
        {
            float delta = e.X - m_ResizeStartX;
            float minW = 36.0f;
            if (m_ResizeColumnIndex == 0) minW = 72.0f;
            else if (m_ResizeColumnIndex == 1) minW = 36.0f;
            else if (m_ResizeColumnIndex == 2) minW = 40.0f;
            else if (m_ResizeColumnIndex == 3) minW = 60.0f;
            else if (m_ResizeColumnIndex == 4) minW = 48.0f;
            else if (m_ResizeColumnIndex == 5) minW = 40.0f;
            float next = std::max(minW, m_ResizeStartWidth + delta);
            if (m_ColumnWidths[(size_t)m_ResizeColumnIndex] != next)
            {
                m_ColumnWidths[(size_t)m_ResizeColumnIndex] = next;
                ApplyColumnWidths();
            }
            m_ResizeStartX = e.X;
            m_ResizeStartWidth = next;
            e.Stop();
            return;
        }
    });
    m_Header->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
        if (e.Button != 0)
            return;
        if (m_IsResizingColumn)
        {
            m_IsResizingColumn = false;
            m_ResizeColumnIndex = -1;
            ApplyColumnWidths();
            e.Stop();
            return;
        }
        if (m_IsDraggingHeader && m_Header && m_DragHeaderVisualIndex >= 0 && m_DragHeaderVisualIndex < (int)m_ColumnOrder.size())
        {
            const auto& children = m_Header->GetChildren();
            int dropVisual = -1;
            for (size_t i = 0; i < children.size(); ++i)
            {
                UIElement* cell = children[i].get();
                float cx = cell->GetLayoutX();
                float cw = cell->GetLayoutWidth();
                if (e.X >= cx && e.X <= cx + cw)
                {
                    dropVisual = (int)i;
                    break;
                }
            }
            const int dragIdx = m_DragHeaderVisualIndex;
            const bool doReorder = (dropVisual >= 0 && dropVisual != dragIdx && dropVisual < (int)m_ColumnOrder.size());
            const bool doSortClick = (dropVisual >= 0 && dropVisual == dragIdx);
            const int sortLogicalCol = (dropVisual >= 0 && dropVisual < (int)m_ColumnOrder.size()) ? m_ColumnOrder[(size_t)dropVisual] : -1;

            m_IsDraggingHeader = false;
            m_DragHeaderVisualIndex = -1;
            e.Stop();

            if (doReorder)
            {
                int logicalCol = m_ColumnOrder[(size_t)dragIdx];
                m_ColumnOrder.erase(m_ColumnOrder.begin() + dragIdx);
                const int insertAt = (dropVisual > dragIdx) ? (dropVisual - 1) : dropVisual;
                m_ColumnOrder.insert(m_ColumnOrder.begin() + insertAt, logicalCol);
                /* Defer rebuild so we're not in event dispatch (RemoveChild would defer and leave stale children). */
                m_Header->PostAction([this]() {
                    if (!m_Header || !m_RowsContainer)
                        return;
                    RebuildHeader();
                    RebuildRows();
                    ApplyColumnWidths();
                });
            }
            else if (doSortClick && sortLogicalCol >= 0 && sortLogicalCol < (int)kBuildPanelColumnCount)
            {
                m_Header->PostAction([this, sortLogicalCol]() {
                    if (!m_Header || !m_RowsContainer)
                        return;
                    OnBuildHeaderClicked(sortLogicalCol);
                });
            }
        }
    });

    scrollContent->AddChild(std::move(header));
    RebuildHeader();
    ApplyColumnWidths();

    /* Initial sort by Platform ascending so sort indicator matches order */
    std::sort(m_Platforms.begin(), m_Platforms.end(), [](const BuildPlatformEntry& a, const BuildPlatformEntry& b) {
        return a.Name.compare(b.Name) < 0;
    });
    UpdateBuildHeaderSortIndicators();

    auto rowsContainer = std::make_unique<UIElement>();
    rowsContainer->AddClass("build-panel-rows");
    m_RowsContainer = rowsContainer.get();
    scrollContent->AddChild(std::move(rowsContainer));
    scroll->AddContent(std::move(scrollContent));
    container->AddChild(std::move(scroll));

    RebuildRows();

    // Output folder row (between platform list and footer)
    {
        auto outputRow = std::make_unique<UIElement>();
        outputRow->AddClass("build-panel-output-row");
        outputRow->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::FlexWrap, false)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::Gap, StyleLength::Px(8.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(12.0f))
            .Set(Style::PaddingRight, StyleLength::Px(12.0f))
            .Set(Style::PaddingTop, StyleLength::Px(8.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(4.0f));

        auto outputLabel = std::make_unique<Label>();
        outputLabel->SetText("Build path:");
        outputLabel->AddClass("build-panel-output-label");
        outputLabel->Overrides()
            .Set(Style::MinWidth, StyleLength::Px(52.0f))
            .Set(Style::Height, StyleLength::Px(kBuildPanelOutputControlHeightPx))
            .Set(Style::MinHeight, StyleLength::Px(kBuildPanelOutputControlHeightPx))
            .Set(Style::MaxHeight, StyleLength::Px(kBuildPanelOutputControlHeightPx))
            .Set(Style::FontSize, StyleLength::Px(kBuildPanelOutputFontPx));
        outputRow->AddChild(std::move(outputLabel));

        // Load current output dir from settings
        std::string currentOutputDir(Editor::kDefaultBuildFolderName);
        {
            // Use the first enabled platform's key (or fallback to first platform)
            std::string platformKey;
            for (const auto& p : m_Platforms)
                if (p.Enabled) { platformKey = p.Name; break; }
            if (platformKey.empty() && !m_Platforms.empty())
                platformKey = m_Platforms[0].Name;
            if (!platformKey.empty())
            {
                std::string key = "build.platform." + platformKey + ".outputDir";
                const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
                auto prefs = root.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(root);
                std::string err;
                prefs.Load(&err);
                std::string val;
                if (prefs.TryGetString(key, val) && !val.empty())
                    currentOutputDir = val;
            }
        }

        auto outputField = std::make_unique<TextField>();
        outputField->SetValue(currentOutputDir);
        outputField->SetTooltip("Output folder used for generated build artifacts.");
        outputField->AddClass("build-panel-output-field");
        outputField->Overrides()
            .Set(Style::FlexGrow, 1.0f)
            .Set(Style::MinWidth, StyleLength::Px(0.0f))
            .Set(Style::Height, StyleLength::Px(kBuildPanelOutputControlHeightPx))
            .Set(Style::MinHeight, StyleLength::Px(kBuildPanelOutputControlHeightPx))
            .Set(Style::MaxHeight, StyleLength::Px(kBuildPanelOutputControlHeightPx))
            .Set(Style::FontSize, StyleLength::Px(kBuildPanelOutputFontPx));
        TextField* outputFieldPtr = outputField.get();
        m_OutputPathField = outputFieldPtr;
        outputRow->AddChild(std::move(outputField));

        auto browseButton = std::make_unique<Button>();
        browseButton->SetText("Browse");
        browseButton->SetTooltip("Choose build output folder");
        browseButton->AddClass("build-panel-output-browse-button");
        browseButton->Overrides()
            .Set(Style::Width, StyleLength::Px(68.0f))
            .Set(Style::MinWidth, StyleLength::Px(68.0f))
            .Set(Style::MaxWidth, StyleLength::Px(68.0f))
            .Set(Style::Height, StyleLength::Px(kBuildPanelOutputControlHeightPx))
            .Set(Style::MinHeight, StyleLength::Px(kBuildPanelOutputControlHeightPx))
            .Set(Style::MaxHeight, StyleLength::Px(kBuildPanelOutputControlHeightPx))
            .Set(Style::FontSize, StyleLength::Px(kBuildPanelOutputFontPx));
        browseButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { BrowseOutputDirectory(); });
        outputRow->AddChild(std::move(browseButton));

        // Save to all platforms when changed (Build panel output dir is a global setting;
        // per-platform granularity is available in the Settings panel).
        outputFieldPtr->RegisterEventHandler(kEventFocusOut, [this, outputFieldPtr](UIEvent&) {
            SaveOutputDirectoryForAllPlatforms(outputFieldPtr->GetValue());
        });

        container->AddChild(std::move(outputRow));
    }

    // Build status label
    {
        auto statusRow = std::make_unique<UIElement>();
        statusRow->Overrides()
            .Set(Style::PaddingLeft, StyleLength::Px(12.0f))
            .Set(Style::PaddingRight, StyleLength::Px(12.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(4.0f));

        auto statusLabel = std::make_unique<Label>();
        statusLabel->AddClass("build-panel-status-label");
        statusLabel->Overrides()
            .Set(Style::FontSize, StyleLength::Px(11.0f))
            .Set(Style::Opacity, 0.6f);
        m_StatusLabel = statusLabel.get();
        statusRow->AddChild(std::move(statusLabel));
        container->AddChild(std::move(statusRow));
    }

    // Footer: Settings button (left), Build button (right)
    auto footer = std::make_unique<UIElement>();
    footer->AddClass("build-panel-footer");
    auto settingsBtn = std::make_unique<Button>();
    settingsBtn->AddClass("build-panel-settings-button");
    settingsBtn->SetText("Settings");
    settingsBtn->SetTooltip("Open build settings");
    settingsBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_OnOpenBuildSettings)
            m_OnOpenBuildSettings();
    });
    footer->AddChild(std::move(settingsBtn));
    auto buildBtn = std::make_unique<Button>();
    buildBtn->AddClass("build-panel-action-button");
    buildBtn->SetText("Build");
    buildBtn->SetTooltip("Build the enabled platforms");
    m_BuildButton = buildBtn.get();
    buildBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        if (m_BuildRunning.load())
            OnCancelClicked();
        else
            OnBuildClicked();
    });
    footer->AddChild(std::move(buildBtn));
    container->AddChild(std::move(footer));

    AddChild(std::move(container));
}

static const char* GetHeaderLabel(int logicalCol)
{
    switch (logicalCol) {
        case kBuildColumnPlatform: return "Platform";
        case kBuildColumnVersion: return "Version";
        case kBuildColumnEnabled: return "On";
        case kBuildColumnProgress: return "Progress";
        case kBuildColumnCompleted: return "Done";
        case kBuildColumnTime: return "Time";
        default: return "";
    }
}

static const char* GetHeaderCellClass(int logicalCol)
{
    switch (logicalCol) {
        case kBuildColumnPlatform: return "build-panel-header-platform";
        case kBuildColumnVersion: return "build-panel-header-version";
        case kBuildColumnEnabled: return "build-panel-header-enabled";
        case kBuildColumnProgress: return "build-panel-header-progress";
        case kBuildColumnCompleted: return "build-panel-header-completed";
        case kBuildColumnTime: return "build-panel-header-time";
        default: return "";
    }
}

static std::string FormatBuildDuration(double seconds)
{
    if (seconds <= 0.0)
        return "--";

    const auto totalSeconds = static_cast<long long>(seconds);
    const long long hours = totalSeconds / 3600;
    const long long minutes = (totalSeconds / 60) % 60;
    const long long secs = totalSeconds % 60;

    std::ostringstream oss;
    oss << std::setfill('0');
    if (hours > 0)
        oss << hours << ':' << std::setw(2) << minutes << ':' << std::setw(2) << secs;
    else
        oss << std::setw(2) << minutes << ':' << std::setw(2) << secs;
    return oss.str();
}

void BuildPanel::RebuildHeader()
{
    if (UIElement::IsInEventDispatch())
    {
        if (m_RebuildHeaderPosted)
            return;
        m_RebuildHeaderPosted = true;
        this->PostAction([this]() {
            m_RebuildHeaderPosted = false;
            RebuildHeader();
        });
        return;
    }

    m_RebuildHeaderPosted = false;

    if (!m_Header || m_ColumnOrder.size() != kBuildPanelColumnCount)
        return;
    m_Header->RemoveAllChildren();
    m_HeaderCells.clear();
    m_HeaderCells.reserve(kBuildPanelColumnCount);
    m_HeaderSortIcons.clear();
    m_HeaderSortIcons.reserve(kBuildPanelColumnCount);

    for (size_t vi = 0; vi < kBuildPanelColumnCount; ++vi)
    {
        int logicalCol = m_ColumnOrder[vi];
        bool hasDivider = (vi + 1u < kBuildPanelColumnCount);

        auto cell = std::make_unique<UIElement>();
        cell->AddClass("build-panel-header-cell");
        cell->AddClass(GetHeaderCellClass(logicalCol));
        auto label = std::make_unique<Label>();
        label->AddClass("header-label");
        label->SetText(GetHeaderLabel(logicalCol));
        cell->AddChild(std::move(label));

        auto sortIcon = std::make_unique<UIElement>();
        sortIcon->AddClass("header-sort-icon");
        {
            sortIcon->Overrides().Set(Style::Display, DisplayMode::None);
        }
        m_HeaderSortIcons.push_back(sortIcon.get());
        cell->AddChild(std::move(sortIcon));

        if (hasDivider && logicalCol >= 0 && logicalCol < (int)m_ColumnWidths.size())
        {
            auto divider = std::make_unique<UIElement>();
            divider->AddClass("build-panel-header-divider");
            int idx = logicalCol;
            divider->RegisterEventHandler(kEventMouseDown, [this, idx](UIEvent& e) {
                if (e.Button != 0 || idx < 0 || idx >= (int)m_ColumnWidths.size())
                    return;
                m_IsResizingColumn = true;
                m_ResizeColumnIndex = idx;
                m_ResizeStartX = e.X;
                m_ResizeStartWidth = m_ColumnWidths[(size_t)idx];
                ApplyColumnWidths();
                e.Capture(m_Header);
                e.Stop();
            });
            cell->AddChild(std::move(divider));
        }

        int visualIndex = (int)vi;
        cell->RegisterEventHandler(kEventMouseDown, [this, visualIndex](UIEvent& e) {
            if (e.Button != 0 || m_IsResizingColumn)
                return;
            m_IsDraggingHeader = true;
            m_DragHeaderVisualIndex = visualIndex;
            e.Capture(m_Header);
        });
        cell->AddClass("build-panel-header-draggable");

        m_HeaderCells.push_back(cell.get());
        m_Header->AddChild(std::move(cell));
    }
    UpdateBuildHeaderSortIndicators();
}

int BuildPanel::VisualIndexForLogicalColumn(int logicalCol) const
{
    for (size_t i = 0; i < m_ColumnOrder.size(); ++i)
        if (m_ColumnOrder[i] == logicalCol)
            return (int)i;
    return -1;
}

void BuildPanel::RebuildRows()
{
    if (UIElement::IsInEventDispatch())
    {
        if (m_RebuildRowsPosted)
            return;
        m_RebuildRowsPosted = true;
        this->PostAction([this]() {
            m_RebuildRowsPosted = false;
            RebuildRows();
        });
        return;
    }

    m_RebuildRowsPosted = false;

    if (!m_RowsContainer || m_ColumnOrder.size() != kBuildPanelColumnCount)
        return;

    m_RowsContainer->RemoveAllChildren();

    const int enabledVisualIndex = VisualIndexForLogicalColumn(2);

    for (size_t rowIndex = 0; rowIndex < m_Platforms.size(); ++rowIndex)
    {
        const BuildPlatformEntry& entry = m_Platforms[rowIndex];

        auto row = std::make_unique<UIElement>();
        row->AddClass("build-panel-row");
        if (entry.Enabled)
            row->AddClass("build-panel-row-enabled");
        // Bold the label of every platform an exporter is registered for
        // (.build-panel-row-selected); the roster itself stays GetBuildPlatforms().
        Editor::BuildExporterDescriptor exporter{};
        if (Editor::BuildExporterRegistry::Get().TryFindForPlatform(entry.Name, exporter))
            row->AddClass("build-panel-row-selected");

        row->Overrides()
            .Set(Style::MinHeight, StyleLength::Px(24.0f))
            .Set(Style::Height, StyleLength::Px(24.0f));

        for (size_t vi = 0; vi < kBuildPanelColumnCount; ++vi)
        {
            int logicalCol = m_ColumnOrder[vi];
            std::unique_ptr<UIElement> cell;
            switch (logicalCol)
            {
            case kBuildColumnPlatform: {
                cell = std::make_unique<UIElement>();
                cell->AddClass("build-panel-row-cell");
                cell->AddClass("build-panel-col-platform");
                auto platformLabel = std::make_unique<Label>();
                platformLabel->SetText(BuildPlatformDisplayName(entry.Name));
                cell->AddChild(std::move(platformLabel));
                break;
            }
            case kBuildColumnVersion: {
                cell = std::make_unique<UIElement>();
                cell->AddClass("build-panel-row-cell");
                cell->AddClass("build-panel-col-version");
                auto versionLabel = std::make_unique<Label>();
                versionLabel->SetText(entry.Version.empty() ? "." : entry.Version);
                cell->AddChild(std::move(versionLabel));
                break;
            }
            case kBuildColumnEnabled: {
                cell = std::make_unique<UIElement>();
                cell->AddClass("build-panel-row-cell");
                cell->AddClass("build-panel-col-enabled");
                /* Use a plain element for the checkbox so it doesn't consume the click (Button would e.Stop() and prevent disable from working) */
                auto enabledBox = std::make_unique<UIElement>();
                enabledBox->AddClass("build-panel-completed-checkbox");
                if (entry.Enabled)
                    enabledBox->AddClass("todo-checkbox-checked");
                int enabledVi = enabledVisualIndex;
                cell->RegisterEventHandler(kEventMouseDown, [this, rowIndex, enabledVi](UIEvent& e) {
                    if (e.Button != 0 || rowIndex >= m_Platforms.size())
                        return;
                    BuildPlatformEntry& platform = m_Platforms[rowIndex];
                    platform.Enabled = !platform.Enabled;
                    const bool enabled = platform.Enabled;
                    if (!m_RowsContainer)
                        return;
                    const auto& rows = m_RowsContainer->GetChildren();
                    if (rowIndex >= rows.size())
                        return;
                    UIElement* rowEl = rows[rowIndex].get();
                    if (enabled)
                        rowEl->AddClass("build-panel-row-enabled");
                    else
                        rowEl->RemoveClass("build-panel-row-enabled");
                    const auto& rowCells = rowEl->GetChildren();
                    if (enabledVi >= 0 && enabledVi < (int)rowCells.size() && !rowCells[(size_t)enabledVi]->GetChildren().empty())
                    {
                        UIElement* box = rowCells[(size_t)enabledVi]->GetChildren()[0].get();
                        if (enabled)
                            box->AddClass("todo-checkbox-checked");
                        else
                            box->RemoveClass("todo-checkbox-checked");
                    }
                    MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
                    e.Stop();
                });
                cell->AddChild(std::move(enabledBox));
                break;
            }
            case kBuildColumnProgress: {
                cell = std::make_unique<UIElement>();
                cell->AddClass("build-panel-row-cell");
                cell->AddClass("build-panel-col-progress");
                auto track = std::make_unique<UIElement>();
                track->AddClass("build-panel-progress-track");
                auto fill = std::make_unique<UIElement>();
                fill->AddClass("build-panel-progress-fill");
                fill->Overrides()
                    .Set(Style::Width, StyleLength::Percent(entry.Progress * 100.0f))
                    .Set(Style::FlexGrow, 0.0f)
                    .Set(Style::FlexShrink, 0.0f);
                track->AddChild(std::move(fill));
                cell->AddChild(std::move(track));
                break;
            }
            case kBuildColumnCompleted: {
                cell = std::make_unique<UIElement>();
                cell->AddClass("build-panel-row-cell");
                cell->AddClass("build-panel-col-completed");
                auto completedDot = std::make_unique<UIElement>();
                completedDot->AddClass("build-panel-enabled-dot");
                if (entry.Completed)
                    completedDot->AddClass("enabled");
                cell->AddChild(std::move(completedDot));
                break;
            }
            case kBuildColumnTime: {
                cell = std::make_unique<UIElement>();
                cell->AddClass("build-panel-row-cell");
                cell->AddClass("build-panel-col-time");
                auto timeLabel = std::make_unique<Label>();
                timeLabel->SetText(FormatBuildDuration(entry.BuildElapsedSeconds));
                cell->AddChild(std::move(timeLabel));
                break;
            }
            default:
                cell = std::make_unique<UIElement>();
                cell->AddClass("build-panel-row-cell");
                break;
            }
            row->AddChild(std::move(cell));
        }

        m_RowsContainer->AddChild(std::move(row));
    }
    ApplyColumnWidths();
}

void BuildPanel::OnBuildHeaderClicked(int logicalCol)
{
    if (logicalCol < 0 || logicalCol >= (int)kBuildPanelColumnCount)
        return;
    if (m_SortColumn == logicalCol)
        m_SortAscending = !m_SortAscending;
    else
    {
        m_SortColumn = logicalCol;
        m_SortAscending = true;
    }

    const bool asc = m_SortAscending;
    switch (m_SortColumn)
    {
    case kBuildColumnPlatform:
        std::sort(m_Platforms.begin(), m_Platforms.end(), [asc](const BuildPlatformEntry& a, const BuildPlatformEntry& b) {
            int c = a.Name.compare(b.Name);
            return asc ? (c < 0) : (c > 0);
        });
        break;
    case kBuildColumnVersion:
        std::sort(m_Platforms.begin(), m_Platforms.end(), [asc](const BuildPlatformEntry& a, const BuildPlatformEntry& b) {
            int c = a.Version.compare(b.Version);
            return asc ? (c < 0) : (c > 0);
        });
        break;
    case kBuildColumnEnabled:
        std::sort(m_Platforms.begin(), m_Platforms.end(), [asc](const BuildPlatformEntry& a, const BuildPlatformEntry& b) {
            return asc ? (a.Enabled < b.Enabled) : (a.Enabled > b.Enabled);
        });
        break;
    case kBuildColumnProgress:
        std::sort(m_Platforms.begin(), m_Platforms.end(), [asc](const BuildPlatformEntry& a, const BuildPlatformEntry& b) {
            return asc ? (a.Progress < b.Progress) : (a.Progress > b.Progress);
        });
        break;
    case kBuildColumnCompleted:
        std::sort(m_Platforms.begin(), m_Platforms.end(), [asc](const BuildPlatformEntry& a, const BuildPlatformEntry& b) {
            return asc ? (a.Completed < b.Completed) : (a.Completed > b.Completed);
        });
        break;
    case kBuildColumnTime:
        std::sort(m_Platforms.begin(), m_Platforms.end(), [asc](const BuildPlatformEntry& a, const BuildPlatformEntry& b) {
            return asc ? (a.BuildElapsedSeconds < b.BuildElapsedSeconds) : (a.BuildElapsedSeconds > b.BuildElapsedSeconds);
        });
        break;
    default:
        break;
    }

    UpdateBuildHeaderSortIndicators();
    RebuildRows();
}

void BuildPanel::UpdateBuildHeaderSortIndicators()
{
    if (m_HeaderSortIcons.size() != m_HeaderCells.size() || m_ColumnOrder.size() != kBuildPanelColumnCount)
        return;
    for (size_t vi = 0; vi < kBuildPanelColumnCount && vi < m_HeaderSortIcons.size() && vi < m_HeaderCells.size(); ++vi)
    {
        int logicalCol = m_ColumnOrder[vi];
        UIElement* icon = m_HeaderSortIcons[vi];
        UIElement* cell = m_HeaderCells[vi];
        if (!icon || !cell)
            continue;
        if (logicalCol == m_SortColumn)
        {
            icon->Overrides().Set(Style::Display, DisplayMode::Block);
            cell->AddClass("sorted");
            if (m_SortAscending)
            {
                icon->AddClass("ascending");
                icon->RemoveClass("descending");
            }
            else
            {
                icon->AddClass("descending");
                icon->RemoveClass("ascending");
            }
        }
        else
        {
            icon->Overrides().Set(Style::Display, DisplayMode::None);
            cell->RemoveClass("sorted");
            icon->RemoveClass("ascending");
            icon->RemoveClass("descending");
        }
    }
}

void BuildPanel::ApplyColumnWidths()
{
    auto apply = [](UIElement* el, float width) {
        if (!el || width <= 0.0f)
            return;
        el->Overrides()
            .Set(Style::FlexGrow, 0.0f)
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::FlexBasis, StyleLength::Px(width))
            .Set(Style::MinWidth, StyleLength::Px(width))
            .Set(Style::MaxWidth, StyleLength::Px(width));
    };

    if (m_ColumnWidths.size() >= kBuildPanelColumnCount && m_ColumnOrder.size() >= kBuildPanelColumnCount && m_HeaderCells.size() >= kBuildPanelColumnCount)
    {
        for (size_t vi = 0; vi < kBuildPanelColumnCount; ++vi)
            apply(m_HeaderCells[vi], m_ColumnWidths[(size_t)m_ColumnOrder[vi]]);
    }

    if (!m_RowsContainer || m_ColumnOrder.size() < kBuildPanelColumnCount || m_ColumnWidths.size() < kBuildPanelColumnCount)
        return;
    for (const auto& rowPtr : m_RowsContainer->GetChildren())
    {
        UIElement* row = rowPtr.get();
        const auto& cells = row->GetChildren();
        if (cells.size() >= kBuildPanelColumnCount)
        {
            for (size_t vi = 0; vi < kBuildPanelColumnCount; ++vi)
                apply(cells[vi].get(), m_ColumnWidths[(size_t)m_ColumnOrder[vi]]);
        }
    }

    /* Force layout recompute so progress bar (width: 100% of cell) updates when column size changes */
    MarkDirty(UIElement::LayoutDirty | UIElement::StyleDirty);
}

// ---------------------------------------------------------------------------
// Build Pipeline Integration
// ---------------------------------------------------------------------------

// Helper: load build scenes for the first enabled platform.
static std::vector<std::string> ParseSceneList(const std::string& raw)
{
    if (raw.empty())
        return {};
    std::vector<std::string> out;
    for (size_t start = 0; start < raw.size(); )
    {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos)
            end = raw.size();
        std::string line = raw.substr(start, end - start);
        // Trim whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t'))
            ++s;
        if (s < line.size())
            line = line.substr(s);
        if (!line.empty())
            out.push_back(std::move(line));
        start = end + (end < raw.size() ? 1u : 0u);
    }
    return out;
}

static std::string LoadBuildIconForPlatform(const std::string& platformName)
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    auto prefs = root.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(root);
    std::string err;
    prefs.Load(&err);

    const std::string useGlobalKey = "build.platform." + platformName + ".useGlobalIcon";
    bool useGlobal = true;
    prefs.TryGetBool(useGlobalKey, useGlobal);

    std::string iconPath;
    if (useGlobal)
    {
        prefs.TryGetString("build.globalIcon", iconPath);
        return iconPath;
    }

    const std::string platformKey = "build.platform." + platformName + ".icon";
    prefs.TryGetString(platformKey, iconPath);
    return iconPath;
}

static std::vector<std::string> LoadBuildScenesForPlatform(const std::string& platformName)
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    auto prefs = root.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(root);
    std::string err;
    prefs.Load(&err);

    // Check if this platform uses the global scene list (default: true).
    std::string useGlobalKey = "build.platform." + platformName + ".useGlobalScenes";
    bool useGlobal = true;
    prefs.TryGetBool(useGlobalKey, useGlobal);

    if (useGlobal)
    {
        // Read from global scene list.
        std::string raw;
        prefs.TryGetString("build.globalScenes", raw);
        auto scenes = ParseSceneList(raw);
        if (!scenes.empty())
            return scenes;
    }

    // Read from platform-specific scene list.
    std::string raw;
    std::string prefKey = "build.platform." + platformName + ".scenes";
    prefs.TryGetString(prefKey, raw);
    return ParseSceneList(raw);
}

static bool RelativePathStartsWithParentTraversal(const std::filesystem::path& rel)
{
    auto it = rel.begin();
    return it != rel.end() && it->string() == "..";
}

static std::string BuildOutputSettingValueForPath(const std::filesystem::path& selectedPath,
                                                  const std::filesystem::path& workspaceRoot)
{
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(selectedPath, ec);
    if (ec)
    {
        ec.clear();
        normalized = std::filesystem::absolute(selectedPath, ec);
    }
    if (ec)
        normalized = selectedPath.lexically_normal();

    if (!workspaceRoot.empty())
    {
        ec.clear();
        const auto rel = std::filesystem::relative(normalized, workspaceRoot, ec);
        if (!ec && !rel.empty() && !RelativePathStartsWithParentTraversal(rel))
            return rel.generic_string();
    }

    return normalized.string();
}

void BuildPanel::SaveOutputDirectoryForAllPlatforms(const std::string& value)
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    auto prefs = root.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(root);
    std::string err;
    prefs.Load(&err);
    for (const auto& p : m_Platforms)
    {
        const std::string key = "build.platform." + p.Name + ".outputDir";
        prefs.SetString(key, value);
    }
    prefs.Save(&err);
}

void BuildPanel::BrowseOutputDirectory()
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    std::filesystem::path initialPath = root;
    if (m_OutputPathField && !m_OutputPathField->GetValue().empty())
    {
        initialPath = std::filesystem::path(m_OutputPathField->GetValue());
        if (initialPath.is_relative() && !root.empty())
            initialPath = root / initialPath;
    }

    std::error_code ec;
    if (!initialPath.empty() && !std::filesystem::is_directory(initialPath, ec))
    {
        const auto parent = initialPath.parent_path();
        if (!parent.empty() && std::filesystem::is_directory(parent, ec))
            initialPath = parent;
    }

    const std::filesystem::path selected = Platform::SelectFolder(initialPath);
    if (selected.empty())
        return;

    const std::string value = BuildOutputSettingValueForPath(selected, root);
    if (m_OutputPathField)
        m_OutputPathField->SetValueWithoutNotify(value);
    SaveOutputDirectoryForAllPlatforms(value);
}

BuildSettings BuildPanel::GatherBuildSettings()
{
    BuildSettings settings;

    auto& engine = EngineCore::GetInstance();
    const auto& wsRoot = engine.GetWorkspaceRoot();

    // Find the first enabled platform and use its scene list.
    for (const auto& plat : m_Platforms)
    {
        if (plat.Enabled)
        {
            settings.platformName = plat.Name;
            settings.scenes = LoadBuildScenesForPlatform(plat.Name);
            break;
        }
    }
    if (settings.platformName.empty() && !m_Platforms.empty())
    {
        // Fallback to first platform if none enabled.
        settings.platformName = m_Platforms[0].Name;
        settings.scenes = LoadBuildScenesForPlatform(settings.platformName);
    }

    settings.applicationIconPath = LoadBuildIconForPlatform(settings.platformName);

    settings.renderPipelinePath = ResolveBuildRenderPipelineForPlatform(settings.platformName);

    // Build output directory from settings (or default).
    {
        std::string outputDirKey = "build.platform." + settings.platformName + ".outputDir";
        std::string outputDir;
        auto prefs = wsRoot.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(wsRoot);
        std::string err;
        prefs.Load(&err);
        prefs.TryGetString(outputDirKey, outputDir);
        if (outputDir.empty())
            outputDir = std::string(Editor::kDefaultBuildFolderName);
        const std::filesystem::path outputPath(outputDir);
        settings.outputDirectory = outputPath.is_absolute()
                                       ? (outputPath / settings.platformName)
                                       : (wsRoot / outputPath / settings.platformName);
    }

    // Project and SDK paths.
    settings.projectRoot = wsRoot;
    settings.editorSDKPath = PathUtils::GetExecutableDirectory() / "SDK";
    settings.runtimeDepsPath = PathUtils::GetExecutableDirectory();

    // Player config derived from project settings.
    GameConfig& cfg = settings.playerConfig;
    // Game name: the "build.gameName" project setting, else the project folder name,
    // else a default. Drives the bundle/exe name, bundle identifier, and game.config.
    {
        std::string gameName;
        auto prefs = wsRoot.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(wsRoot);
        std::string err;
        prefs.Load(&err);
        prefs.TryGetString("build.gameName", gameName);
        if (gameName.empty() && !wsRoot.empty())
            gameName = wsRoot.filename().string();
        if (gameName.empty())
            gameName = "My Game";
        cfg.gameName = gameName;
    }
    cfg.startupScene = settings.scenes.empty() ? "" : settings.scenes[0];
    cfg.renderPipeline = settings.renderPipelinePath;
    cfg.windowWidth = 1920;
    cfg.windowHeight = 1080;
    cfg.windowMode = LoadBuildPlayerWindowMode();
    cfg.vsync = true;
    {
        auto prefs = wsRoot.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(wsRoot);
        std::string err;
        (void)prefs.Load(&err);
        const auto& rootJson = prefs.Json();
        if (rootJson.is_object())
        {
            const auto itRendering = rootJson.find("rendering");
            if (itRendering != rootJson.end() && itRendering->is_object())
            {
                const auto itHdr = itRendering->find("hdr");
                if (itHdr != itRendering->end() && itHdr->is_object())
                {
                    const auto& hdr = *itHdr;
                    if (hdr.contains("enabled") && hdr["enabled"].is_boolean())
                        cfg.hdrEnabled = hdr["enabled"].get<bool>();
                    if (hdr.contains("mode") && hdr["mode"].is_string())
                        cfg.hdrMode = Rendering::HdrOutputModeFromString(hdr["mode"].get<std::string>());
                    if (hdr.contains("swapchainBitDepth"))
                    {
                        if (hdr["swapchainBitDepth"].is_number_integer() && hdr["swapchainBitDepth"].get<int>() == 16)
                            cfg.hdrSwapchainBitDepth = Rendering::HdrSwapchainBitDepth::Float16;
                        else if (hdr["swapchainBitDepth"].is_string())
                        {
                            std::string bitDepth = hdr["swapchainBitDepth"].get<std::string>();
                            std::transform(bitDepth.begin(), bitDepth.end(), bitDepth.begin(), [](unsigned char c) {
                                return static_cast<char>(std::tolower(c));
                            });
                            bitDepth.erase(std::remove_if(bitDepth.begin(), bitDepth.end(), [](char c) {
                                return c == '-' || c == '_' || c == ' ';
                            }), bitDepth.end());
                            cfg.hdrSwapchainBitDepth = (bitDepth == "16" || bitDepth == "float16" || bitDepth == "fp16" || bitDepth == "rgba16f")
                                                           ? Rendering::HdrSwapchainBitDepth::Float16
                                                           : Rendering::HdrSwapchainBitDepth::Bit10;
                        }
                        else
                            cfg.hdrSwapchainBitDepth = Rendering::HdrSwapchainBitDepth::Bit10;
                    }
                    if (hdr.contains("targetDisplay") && hdr["targetDisplay"].is_number_integer())
                        cfg.hdrTargetDisplay = hdr["targetDisplay"].get<int>();
                    if (hdr.contains("metadata") && hdr["metadata"].is_object())
                    {
                        const auto& metadata = hdr["metadata"];
                        auto readFloat = [&](const char* key, float& outValue) {
                            if (metadata.contains(key) && metadata[key].is_number())
                                outValue = metadata[key].get<float>();
                        };
                        readFloat("maxMasteringLuminance", cfg.hdrStaticMetadata.maxMasteringLuminance);
                        readFloat("minMasteringLuminance", cfg.hdrStaticMetadata.minMasteringLuminance);
                        readFloat("maxContentLightLevel", cfg.hdrStaticMetadata.maxContentLightLevel);
                        readFloat("maxFrameAverageLightLevel", cfg.hdrStaticMetadata.maxFrameAverageLightLevel);
                        readFloat("paperWhiteNits", cfg.hdrStaticMetadata.paperWhiteNits);
                    }
                }
            }
        }
    }

    // The executable owns custom application sources only. Gameplay native
    // sources elsewhere in Assets are built and staged as the user module.
    settings.userCppSources = GameEngine::CollectCustomPlayerSources(engine.GetResolvedAssetRoot());
    std::error_code ec;
    settings.compileScripts = true;

    // Build configuration from settings. The SDK stores libs per-config
    // (SDK/lib/Debug/, SDK/lib/Release/), so the user can choose; an unset
    // project takes the same default the settings UI displays, so the
    // configuration shown is the configuration built.
    {
        std::string configKey = "build.platform." + settings.platformName + ".buildConfig";
        std::string buildConfig;
        auto prefs = wsRoot.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(wsRoot);
        std::string err;
        prefs.Load(&err);
        prefs.TryGetString(configKey, buildConfig);
        if (buildConfig.empty())
            buildConfig = std::string(DefaultPlayerBuildConfig());
        settings.buildConfiguration = buildConfig;
    }

    // Detect cmake generator. Prefer the generator used to build the Editor itself.
    // CMAKE_GENERATOR is baked at configure time via compile definition (if available),
    // otherwise fall back to a reasonable default.
#ifdef _WIN32
    // Try VS 2026 first (18.x), then VS 2022 (17.x). cmake --help shows available generators.
    settings.cmakeGenerator = ""; // empty = let cmake pick the default
#else
    settings.cmakeGenerator = "Ninja";
#endif

    // Engine version for SDK compatibility check.
    settings.engineVersion = kEngineVersion;

    // Vcpkg toolchain — look for it relative to the Editor's SDK or project root.
    // The Engine build tree has dependencies/vcpkg/ at repo root.
    auto vcpkgCandidate = wsRoot / "dependencies" / "vcpkg" / "scripts" / "buildsystems" / "vcpkg.cmake";
    if (std::filesystem::exists(vcpkgCandidate, ec))
        settings.vcpkgToolchainFile = vcpkgCandidate;

    // vcpkg installed tree, optional: the Player links against the staged SDK alone,
    // and the runtime DLLs of the editor's own CRT flavor ship from beside the editor.
    // The tree supplies the other flavor's runtime DLLs for a cross-flavor package
    // (refused, naming this key, when absent) and extra Player link inputs.
    // Resolution: the explicit build.vcpkgInstalledDir setting first, then the dev
    // build tree the Editor itself runs from (vcpkg_installed sits at the preset build
    // root above the exe, or one level further up when presets share it). The second
    // probe leaves the staged output on purpose and is DEV-ONLY: the tree is never
    // staged. A packaged Editor with neither leaves this empty.
    {
        std::string vcpkgInstalledSetting;
        auto prefs = wsRoot.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(wsRoot);
        std::string err;
        prefs.Load(&err);
        prefs.TryGetString("build.vcpkgInstalledDir", vcpkgInstalledSetting);
        if (!vcpkgInstalledSetting.empty() && std::filesystem::is_directory(vcpkgInstalledSetting, ec))
        {
            settings.vcpkgInstalledDir = vcpkgInstalledSetting;
        }
        else
        {
            const std::filesystem::path exeDir = PathUtils::GetExecutableDirectory();
            for (const std::filesystem::path& candidate :
                 {exeDir / "../../../../vcpkg_installed", exeDir / "../../../../../vcpkg_installed"})
            {
                if (std::filesystem::is_directory(candidate, ec))
                {
                    settings.vcpkgInstalledDir = std::filesystem::weakly_canonical(candidate, ec);
                    break;
                }
            }
        }
    }

    return settings;
}

void BuildPanel::OnBuildClicked()
{
    // Atomically check-and-set to prevent race on rapid double-clicks.
    bool expected = false;
    if (!m_BuildRunning.compare_exchange_strong(expected, true))
    {
        Logger::Log::Warning("Build: A build is already in progress");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_ProgressMutex);
        ++m_BuildGeneration;
        m_BuildSucceeded = false;
        m_LatestProgress = BuildProgress{};
        m_BuildOutputDirectory.clear();
    }

    // The previous export job has cleared the flag we just set; wait for the rest of it.
    if (m_BuildJob.IsValid())
        m_BuildJob.Wait();

    BuildSettings settings = GatherBuildSettings();

    // A platform with a registered exporter runs it instead of BuildPipeline's
    // native compile-and-package phases.
    Editor::BuildExporterDescriptor exporter;
    const bool hasExporter =
        Editor::BuildExporterRegistry::Get().TryFindForPlatform(settings.platformName, exporter);
    {
        std::lock_guard<std::mutex> lock(m_ProgressMutex);
        m_BuildOutputDirectory = hasExporter && exporter.OutputDirectory ? exporter.OutputDirectory(settings)
                                                                         : settings.outputDirectory;
    }

    if (settings.scenes.empty())
    {
        Logger::Log::Warning("Build: No scenes configured for platform '{}'. Add scenes in Build Settings.", settings.platformName);
        if (m_StatusLabel)
            m_StatusLabel->SetText("No scenes configured. Add scenes in Settings > Build > " +
                                   BuildPlatformDisplayName(settings.platformName) + ".");
        {
            std::lock_guard<std::mutex> lock(m_ProgressMutex);
            m_LatestProgress.currentStage = BuildProgress::Stage::Failed;
            m_LatestProgress.statusMessage = "No scenes configured in Build Settings";
            m_BuildRunning.store(false);
        }
        return;
    }

    Logger::Log::Info("Build: Starting build for platform '{}' ({} scene(s))",
                      settings.platformName, settings.scenes.size());

    // m_BuildRunning already set to true by compare_exchange_strong above.
    const auto buildStartedAt = std::chrono::steady_clock::now();
    bool markedAnyPlatform = false;
    for (auto& plat : m_Platforms)
    {
        if (plat.Enabled)
        {
            plat.Completed = false;
            plat.Progress = 0.0f;
            plat.BuildRunning = true;
            plat.BuildElapsedSeconds = 0.0;
            plat.BuildStartedAt = buildStartedAt;
            markedAnyPlatform = true;
        }
    }
    if (!markedAnyPlatform)
    {
        for (auto& plat : m_Platforms)
        {
            if (plat.Name == settings.platformName)
            {
                plat.Completed = false;
                plat.Progress = 0.0f;
                plat.BuildRunning = true;
                plat.BuildElapsedSeconds = 0.0;
                plat.BuildStartedAt = buildStartedAt;
                break;
            }
        }
    }
    RebuildRows();
    UpdateBuildButtonState();
    ScheduleBuildTimerTick();

    m_Pipeline = std::make_unique<BuildPipeline>(EngineCore::GetInstance().GetNativeScriptManager());

    if (!m_ExportChannel)
        m_ExportChannel = std::make_unique<JobSystem::JobChannel>(
            EngineCore::GetInstance().GetJobSystem(), JobSystem::JobChannelDesc{.Name = "Game export", .MaxRunning = 1});
    m_BuildJob = m_ExportChannel->Submit([this, settings = std::move(settings), exporter = std::move(exporter),
                                          hasExporter]() {
        const auto reportProgress = [this](const BuildProgress& progress) { OnBuildProgress(progress); };
        bool ok = hasExporter ? exporter.Run(settings, *m_Pipeline, reportProgress)
                              : m_Pipeline->Execute(settings, reportProgress);

        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_ProgressMutex);
            cancelled = m_LatestProgress.cancelled;
        }
        if (!cancelled && m_Pipeline && m_Pipeline->IsCancelled())
            cancelled = true;

        if (cancelled)
            ok = false;

        {
            std::lock_guard<std::mutex> lock(m_ProgressMutex);
            m_BuildSucceeded = ok && !cancelled;
            m_BuildRunning.store(false);
        }

        // Surface accumulated warnings/errors and the completion verdict from
        // the build thread itself — otherwise a degraded bundle (missing native
        // dylib, zero managed assemblies, missing render pipeline) reports a
        // green "Build complete!" with no diagnostics anywhere. Logger is
        // thread-safe, and doing it here (not in the PostAction) means a build
        // triggered headless/via IPC still lands its diagnostics in the log
        // when the panel is closed and its posted actions are dropped.
        std::vector<std::string> buildWarnings, buildErrors;
        {
            std::lock_guard<std::mutex> lock(m_ProgressMutex);
            buildWarnings = m_LatestProgress.warnings;
            buildErrors = m_LatestProgress.errors;
        }
        for (const auto& w : buildWarnings)
            Logger::Log::Warning("Build: {}", w);
        for (const auto& e : buildErrors)
            Logger::Log::Error("Build: {}", e);
        if (cancelled)
            Logger::Log::Info("Build: Cancelled by user");
        else if (ok)
            Logger::Log::Info("Build: Completed successfully!");
        else
            Logger::Log::Error("Build: Failed. Check log for details.");

        // Post UI update back to main thread (panel-only cosmetics; dropped
        // harmlessly if the panel closed mid-build).
        this->PostAction([this, ok, cancelled,
                          warningCount = buildWarnings.size()]() {
            UpdateBuildButtonState();
            UpdateRunningBuildDurations();

            // Update the platform row: mark as completed.
            for (auto& plat : m_Platforms)
            {
                if (plat.Enabled || plat.BuildRunning)
                {
                    plat.Completed = ok;
                    plat.Progress = ok ? 1.0f : plat.Progress;
                    plat.BuildRunning = false;
                }
            }
            RebuildRows();

            if (m_StatusLabel)
            {
                if (cancelled)
                    m_StatusLabel->SetText("Build cancelled.");
                else if (ok)
                    m_StatusLabel->SetText(warningCount == 0
                                               ? "Build complete!"
                                               : ("Build complete with " +
                                                  std::to_string(warningCount) + " warning(s) — see log."));
                else
                    m_StatusLabel->SetText("Build failed. Check log for details.");
            }
        });
    });
}

BuildPanel::StatusSnapshot BuildPanel::GetBuildStatus()
{
    std::lock_guard<std::mutex> lock(m_ProgressMutex);
    return {m_BuildRunning.load(), m_BuildSucceeded, m_BuildGeneration, m_LatestProgress, m_BuildOutputDirectory};
}

void BuildPanel::OnBuildProgress(const BuildProgress& progress)
{
    {
        std::lock_guard<std::mutex> lock(m_ProgressMutex);
        m_LatestProgress = progress;
    }

    // Post progress UI update to main thread.
    this->PostAction([this]() {
        BuildProgress p;
        {
            std::lock_guard<std::mutex> lock(m_ProgressMutex);
            p = m_LatestProgress;
        }

        // Update progress for enabled platforms.
        UpdateRunningBuildDurations();
        for (auto& plat : m_Platforms)
        {
            if (plat.Enabled)
                plat.Progress = p.progress;
        }
        RebuildRows();

        // Update status label.
        if (m_StatusLabel)
            m_StatusLabel->SetText(p.statusMessage);
    });
}

void BuildPanel::UpdateRunningBuildDurations()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto& plat : m_Platforms)
    {
        if (!plat.BuildRunning || plat.BuildStartedAt == std::chrono::steady_clock::time_point{})
            continue;
        plat.BuildElapsedSeconds = std::chrono::duration<double>(now - plat.BuildStartedAt).count();
    }
}

void BuildPanel::ScheduleBuildTimerTick()
{
    if (m_BuildTimerTickScheduled || !m_BuildRunning.load())
        return;

    auto tick = [this]() {
        m_BuildTimerTickScheduled = false;
        UpdateRunningBuildDurations();
        RebuildRows();
        if (m_BuildRunning.load())
            ScheduleBuildTimerTick();
    };

    if (Scheduler::IScheduler* scheduler = GetScheduler())
    {
        m_BuildTimerTickScheduled = true;
        UIManager* owner = GetOwnerManager();
        const uint64_t instanceId = GetInstanceId();
        scheduler->ScheduleAfter(std::chrono::seconds(1), [owner, instanceId]() {
            if (!owner)
                return;
            UIElement* element = owner->FindElementByInstanceId(instanceId);
            auto* panel = dynamic_cast<BuildPanel*>(element);
            if (!panel)
                return;
            panel->m_BuildTimerTickScheduled = false;
            panel->UpdateRunningBuildDurations();
            panel->RebuildRows();
            if (panel->m_BuildRunning.load())
                panel->ScheduleBuildTimerTick();
        });
        return;
    }

    m_BuildTimerTickScheduled = true;
    PostSafeAction(std::move(tick));
}

void BuildPanel::TriggerBuild()
{
    PostAction([this]() { OnBuildClicked(); });
}

void BuildPanel::TriggerBuildImmediate()
{
    OnBuildClicked();
}

bool BuildPanel::SetOnlyEnabledPlatform(const std::string& platformName)
{
    const std::string requested = ToLowerAscii(platformName);
    // An exporter that answers to several spellings makes them select the same
    // row ("steam deck" enables the stored "Steam" target).
    Editor::BuildExporterDescriptor requestedExporter;
    const bool hasRequestedExporter =
        Editor::BuildExporterRegistry::Get().TryFindForPlatform(platformName, requestedExporter);
    bool found = false;
    for (auto& platform : m_Platforms)
    {
        const bool match = ToLowerAscii(platform.Name) == requested ||
                           (hasRequestedExporter && requestedExporter.HandlesPlatform(platform.Name));
        platform.Enabled = match;
        if (match)
            found = true;
    }

    if (found)
    {
        RebuildRows();
        UpdateBuildButtonState();
    }
    return found;
}

void BuildPanel::OnCancelClicked()
{
    if (!m_BuildRunning.load())
        return;

    if (m_Pipeline)
        m_Pipeline->RequestCancel();

    PostAction([this]() {
        if (m_StatusLabel)
            m_StatusLabel->SetText("Cancelling build...");
    });
}

void BuildPanel::UpdateBuildButtonState()
{
    if (!m_BuildButton)
        return;
    if (m_BuildRunning.load())
        m_BuildButton->SetText("Cancel");
    else
        m_BuildButton->SetText("Build");
}

namespace
{
constexpr const char* kBuildPlayerWindowModePrefKey = "build.player.windowMode";

Editor::SettingsStore OpenBuildPlayerSettingsStore()
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    if (!root.empty())
        return Editor::OpenProjectSettings(root);
    return Editor::OpenEditorPreferences();
}
} // namespace

WindowMode LoadBuildPlayerWindowMode()
{
    std::string storedMode;
    auto prefs = OpenBuildPlayerSettingsStore();
    std::string err;
    prefs.Load(&err);
    if (!prefs.TryGetString(kBuildPlayerWindowModePrefKey, storedMode) || storedMode.empty())
        return WindowMode::Windowed;

    WindowMode parsed = WindowMode::Windowed;
    if (TryParseWindowMode(storedMode, parsed))
        return parsed;
    return WindowMode::Windowed;
}

void SaveBuildPlayerWindowMode(WindowMode mode)
{
    auto prefs = OpenBuildPlayerSettingsStore();
    std::string err;
    prefs.Load(&err);
    prefs.SetString(kBuildPlayerWindowModePrefKey, WindowModeToString(mode));
    prefs.Save(&err);
}

} // namespace GameEngine

#pragma once

#include "UI/Controls/DockPanel.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/GameConfig.h"
#include "JobSystem/TaskHandle.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace JobSystem {
class JobChannel;
}

namespace GameEngine {

class UIElement;
class ScrollView;
class Label;
class Button;
class TextField;

struct BuildPlatformEntry {
    std::string Name;
    std::string Version;
    bool Enabled = false;
    float Progress = 0.0f;  // 0..1
    bool Completed = false;
    bool BuildRunning = false;
    double BuildElapsedSeconds = 0.0;
    std::chrono::steady_clock::time_point BuildStartedAt{};
};

class BuildPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "game-controller-icon"; }

    BuildPanel();
    ~BuildPanel() override;

    /** Reload version from settings and refresh the version column. Call when Build tab is shown or periodically so the panel never shows stale values. */
    void RefreshVersionsFromSettings();

    /** Set callback for the Settings button (opens Settings and shows Build section). Called by the host when wiring panels. */
    void SetOnOpenBuildSettings(std::function<void()> cb) { m_OnOpenBuildSettings = std::move(cb); }

    /** Programmatically trigger a build (used by UI button). Posts to UI thread. */
    void TriggerBuild();

    /** Start a build synchronously on the calling thread. The debug-server handler
     *  already runs on the main thread (via FlushPendingRequests), where PostAction
     *  has no UI dispatcher and would be dropped — so the IPC path calls this directly. */
    void TriggerBuildImmediate();

    /** Enable only the requested platform before a programmatic build. */
    bool SetOnlyEnabledPlatform(const std::string& platformName);

    struct StatusSnapshot
    {
        bool running = false;
        bool succeeded = false;
        uint64_t generation = 0;
        BuildProgress progress;
        std::filesystem::path outputDirectory;
    };
    // A completion receipt for automation; starting a build is not completion.
    StatusSnapshot GetBuildStatus();

private:
    void BuildUI();
    void LoadVersionsFromSettings();
    void RebuildHeader();
    void RebuildRows();
    void ApplyColumnWidths();
    int VisualIndexForLogicalColumn(int logicalCol) const;
    void OnBuildHeaderClicked(int logicalCol);
    void UpdateBuildHeaderSortIndicators();

    // Build pipeline integration
    void OnBuildClicked();
    void OnCancelClicked();
    BuildSettings GatherBuildSettings();
    void OnBuildProgress(const BuildProgress& progress);
    void ScheduleBuildTimerTick();
    void UpdateRunningBuildDurations();
    void UpdateBuildButtonState();
    void SaveOutputDirectoryForAllPlatforms(const std::string& value);
    void BrowseOutputDirectory();

    UIElement* m_MainContainer = nullptr;
    UIElement* m_RowsContainer = nullptr;
    UIElement* m_Header = nullptr;
    ScrollView* m_ScrollView = nullptr;
    Button* m_BuildButton = nullptr;
    TextField* m_OutputPathField = nullptr;
    Label* m_StatusLabel = nullptr;

    std::vector<UIElement*> m_HeaderCells;
    std::vector<UIElement*> m_HeaderSortIcons;

    std::vector<BuildPlatformEntry> m_Platforms;
    std::vector<float> m_ColumnWidths;
    std::vector<int> m_ColumnOrder;

    bool m_IsResizingColumn = false;
    int m_ResizeColumnIndex = -1;
    float m_ResizeStartX = 0.0f;
    float m_ResizeStartWidth = 0.0f;

    bool m_IsDraggingHeader = false;
    int m_DragHeaderVisualIndex = -1;

    int m_SortColumn = 0;       // logical column: 0=Platform, 1=Version, 2=Enabled, 3=Progress, 4=Completed, 5=Time
    bool m_SortAscending = true;
    bool m_RebuildHeaderPosted = false;
    bool m_RebuildRowsPosted = false;
    bool m_BuildTimerTickScheduled = false;

    std::function<void()> m_OnOpenBuildSettings;

    // Build pipeline state
    std::unique_ptr<BuildPipeline> m_Pipeline;
    // "Game export" (cap 1): the export is a job of it on the job system's blocking
    // threads (it waits on compilers, dotnet and the disk for minutes, and forks its
    // own cook work onto the pool). Created by the first build.
    std::unique_ptr<JobSystem::JobChannel> m_ExportChannel;
    // The latest export job; the destructor and the next build wait on it.
    JobSystem::TaskHandle m_BuildJob;
    std::atomic<bool> m_BuildRunning{false};
    std::mutex m_ProgressMutex;
    BuildProgress m_LatestProgress;
    bool m_BuildSucceeded = false; // protected by m_ProgressMutex
    uint64_t m_BuildGeneration = 0;
    std::filesystem::path m_BuildOutputDirectory;
};

/** Call when build version is changed in Settings so the Build panel can refresh (only on value change and first load). */
void NotifyBuildVersionChanged();

WindowMode LoadBuildPlayerWindowMode();
void SaveBuildPlayerWindowMode(WindowMode mode);

} // namespace GameEngine

#pragma once

#include "MovieRecorderController.h"
#include "UI/Controls/DockPanel.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

class Button;
class Dropdown;
class IntField;
class Label;
class ScrollView;
class TextField;
class Toggle;
class UIElement;

namespace Editor
{
class UndoRedoService;
}

class MovieRecorderPanel : public DockPanel, public Editor::IMovieRecorderView
{
public:
    explicit MovieRecorderPanel(bool embeddedInSettings = false);
    ~MovieRecorderPanel() override;

    void SetOnPlayVideo(std::function<void(const std::filesystem::path&, const std::string&)> fn) { m_OnPlayVideo = std::move(fn); }
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_Undo = undo; }
    // Registers this panel as the controller's session view; the first Tick
    // pulls session state that fired while no view was alive.
    void SetController(Editor::MovieRecorderController* controller);
    void RequestRefreshRecentRecordings(const std::filesystem::path& trackedOutputPath = {});

    // Editor::IMovieRecorderView
    void Tick() override;
    void OnRecorderStatus(const std::string& message, bool warning) override;
    void OnRecordingStarted(const std::filesystem::path& path) override;
    void OnRecordingStopRequested(const std::filesystem::path& path) override;
    void OnRecordingFinished(const std::filesystem::path& path) override;
    void OnExternalRecordingFinished(const std::filesystem::path& path) override;
    void OnChoosePathRequested() override;
    void FlushSettingsToStore() override;
    void OnRecorderSettingsChanged() override;

    struct SettingsSnapshot
    {
        std::string Codec;
        std::string Hdr = "off"; // off | pq | hlg
        std::string EncoderMode;
        std::string OutputPath;
        bool OutputPathSet = false;
        int Fps = 60;
        int FrameCount = 240;
        int Width = 1920;
        int Height = 1080;
        int BitrateKbps = 60000;
        bool AutoBitrate = true;
        bool RecordAudio = true;
        bool FadeIn = false;
        bool FadeOut = false;
        std::string FadeColor = "black";
        int FadeDurationSeconds = 1;
    };

private:
    struct RecordingEntry
    {
        std::filesystem::path Path;
        std::filesystem::file_time_type Modified;
        std::uintmax_t SizeBytes = 0;
    };

    struct RecentRefreshState
    {
        std::mutex mutex;
        std::vector<RecordingEntry> Result;
        std::uint64_t Generation = 0;
        bool HasResult = false;
    };

    void BuildUI();
    void LoadSettings();
    void SaveSettings();
    void CommitSettingsChange(const char* actionName);
    SettingsSnapshot CaptureSettingsSnapshot() const;
    void ApplySettingsSnapshot(const SettingsSnapshot& snapshot);
    void ApplyResolutionPreset(const std::string& value);
    void SyncResolutionPresetDropdown();
    void ApplyPresetBitrateDefault();
    void UpdateBitrateAvailability();
    void RefreshRecentRecordings();
    void PumpRecentRecordingsRefresh();
    bool InsertRecentRecordingFromDisk(const std::filesystem::path& path, bool allowInaccessible = false);
    void StartRecentRecordingAutoRefresh(const std::filesystem::path& path, double seconds);
    void PumpRecentRecordingAutoRefresh();
    void StartRecentRecordingListVerification(const std::filesystem::path& path, double seconds);
    void PumpRecentRecordingListVerification();
    bool IsRecentRecordingListed(const std::filesystem::path& path) const;
    bool IsActiveRecordingPath(const std::filesystem::path& path) const;
    void RebuildRecentRows();
    void EnsureDeleteRecordingPrompt();
    void EnsureChoosePathPrompt();
    void ShowDeleteRecordingPrompt(const std::filesystem::path& path);
    void HideDeleteRecordingPrompt();
    void HideChoosePathPrompt();
    void RemoveRecentRecordingRow(const std::filesystem::path& path);
    void DeleteRecentRecordingFile(const std::filesystem::path& path);
    void SetStatusText(const std::string& message, bool warning = false);
    void SetTransientRefreshStatus();
    void RefreshOutputPathStatus();
    void SyncRecordButtonState();
    void SetRecordButtonActive(bool active);
    void BrowseOutputPath();
    void PlayRecording(const std::filesystem::path& path);
    void RevealRecording(const std::filesystem::path& path);

    std::filesystem::path CurrentOutputPath() const;
    std::string SelectedCodec() const;
    std::string SelectedEncoderMode() const;
    const char* CodecFilterPattern() const;
    const char* CodecDefaultExtension() const;
    int ParsePositiveInt(IntField* field, int fallback) const;
    int ParseBitrateKbps(IntField* field, int fallback) const;

    Dropdown* m_CodecDropdown = nullptr;
    Dropdown* m_HdrDropdown = nullptr;
    Dropdown* m_ResolutionPresetDropdown = nullptr;
    Dropdown* m_EncoderDropdown = nullptr;
    TextField* m_OutputPathField = nullptr;
    IntField* m_FpsField = nullptr;
    IntField* m_FrameCountField = nullptr;
    IntField* m_WidthField = nullptr;
    IntField* m_HeightField = nullptr;
    IntField* m_BitrateField = nullptr;
    IntField* m_FadeDurationField = nullptr;
    Toggle* m_AutoBitrateToggle = nullptr;
    Toggle* m_RecordAudioToggle = nullptr;
    Toggle* m_FadeInToggle = nullptr;
    Toggle* m_FadeOutToggle = nullptr;
    Dropdown* m_FadeColorDropdown = nullptr;
    Button* m_RecordButton = nullptr;
    Button* m_BrowseButton = nullptr;
    UIElement* m_StatusRow = nullptr;
    Label* m_StatusLabel = nullptr;
    UIElement* m_RecentRows = nullptr;
    Label* m_ActiveRecordingSizeLabel = nullptr;
    std::string m_ActiveRecordingSizeKey;
    ScrollView* m_PanelScroll = nullptr;
    UIElement* m_DeletePromptOverlay = nullptr;
    Label* m_DeletePromptMessage = nullptr;
    UIElement* m_ChoosePathPromptOverlay = nullptr;
    std::filesystem::path m_PendingDeletePath;

    std::vector<RecordingEntry> m_RecentRecordings;
    std::unordered_set<std::string> m_RemovedRecentRecordingKeys;
    std::optional<SettingsSnapshot> m_LastCommittedSettings;
    Editor::UndoRedoService* m_Undo = nullptr;
    Editor::MovieRecorderController* m_Controller = nullptr;
    std::shared_ptr<bool> m_LifetimeToken = std::make_shared<bool>(true);
    std::shared_ptr<RecentRefreshState> m_RecentRefreshState = std::make_shared<RecentRefreshState>();
    std::uint64_t m_RecentRefreshGeneration = 0;
    std::uint64_t m_AppliedRecentRefreshGeneration = 0;
    bool m_RecentRefreshInFlight = false;
    bool m_RecentRefreshQueued = false;
    std::filesystem::path m_RecentAutoRefreshPath;
    std::filesystem::path m_LastRecordingOutputPath;
    std::chrono::steady_clock::time_point m_RecentAutoRefreshNext;
    std::chrono::steady_clock::time_point m_RecentAutoRefreshDeadline;
    bool m_RecentAutoRefreshActive = false;
    // Last on-disk size observed while polling; finalization is complete once a
    // non-zero size stops growing between polls.
    std::uintmax_t m_RecentAutoRefreshLastSize = 0;
    std::filesystem::path m_RecentListVerificationPath;
    std::chrono::steady_clock::time_point m_RecentListVerificationNext;
    std::chrono::steady_clock::time_point m_RecentListVerificationDeadline;
    std::uint64_t m_RecentListVerificationStartGeneration = 0;
    bool m_RecentListVerificationActive = false;
    // The status label is showing the transient "Refreshing..." note, cleared
    // once the scan it describes lands.
    bool m_StatusShowsRefreshing = false;
    bool m_SuppressSettingUndo = false;
    bool m_OutputPathExplicit = false;
    bool m_IsRecordingActual = false;

    std::function<void(const std::filesystem::path&, const std::string&)> m_OnPlayVideo;
    bool m_EmbeddedInSettings = false;
};

} // namespace GameEngine

#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace GameEngine
{
class GameViewController;

namespace Editor
{
class PlayModeManager;
class UndoRedoService;

// Settings-store keys shared by the recording session (controller) and the
// settings UI (MovieRecorderPanel).
namespace MovieRecorderPrefs
{
inline constexpr const char* kCodec = "movieRecorder.codec";
inline constexpr const char* kHdr = "movieRecorder.hdr";
inline constexpr const char* kFps = "movieRecorder.fps";
inline constexpr const char* kFrameCount = "movieRecorder.frameCount";
inline constexpr const char* kWidth = "movieRecorder.width";
inline constexpr const char* kHeight = "movieRecorder.height";
inline constexpr const char* kOutputPath = "movieRecorder.outputPath";
inline constexpr const char* kOutputPathSet = "movieRecorder.outputPathSet";
inline constexpr const char* kBitrateKbps = "movieRecorder.bitrateKbps";
inline constexpr const char* kAutoBitrate = "movieRecorder.autoBitrate";
inline constexpr const char* kEncoderMode = "movieRecorder.encoderMode";
inline constexpr const char* kRecordAudio = "movieRecorder.recordAudio";
inline constexpr const char* kFadeIn = "movieRecorder.fadeIn";
inline constexpr const char* kFadeOut = "movieRecorder.fadeOut";
inline constexpr const char* kFadeColor = "movieRecorder.fadeColor";
inline constexpr const char* kFadeDurationSeconds = "movieRecorder.fadeDurationSeconds";
} // namespace MovieRecorderPrefs

// Canonical resolution/frame-rate presets shared by the settings UI and the
// toolbar record menu.
struct MovieRecorderResolutionPreset
{
    const char* Value;
    const char* Label;
    int Width;
    int Height;
    int Fps;
    int BitrateKbps;
};
std::span<const MovieRecorderResolutionPreset> GetMovieRecorderResolutionPresets();
const MovieRecorderResolutionPreset* FindMovieRecorderResolutionPreset(const std::string& value);
// The preset value matching (width, height, fps), or "custom".
std::string MovieRecorderResolutionPresetValueFor(int width, int height, int fps);

// A recording-session view (the Settings Recording page). Registered views
// receive session events on the main thread and are ticked by the controller.
// Views are created and destroyed freely mid-session; state that fired while
// no view was alive is pulled via the controller's TakePending* accessors.
class IMovieRecorderView
{
public:
    virtual ~IMovieRecorderView() = default;

    virtual void Tick() = 0;
    virtual void OnRecorderStatus(const std::string& message, bool warning) = 0;
    virtual void OnRecordingStarted(const std::filesystem::path& path) = 0;
    virtual void OnRecordingStopRequested(const std::filesystem::path& path) = 0;
    virtual void OnRecordingFinished(const std::filesystem::path& path) = 0;
    // A recording produced outside the controller session (debug server)
    // finished; refresh recording lists tracking `path`.
    virtual void OnExternalRecordingFinished(const std::filesystem::path& path) = 0;
    // Record was requested without an output path; surface the choose-path UI.
    virtual void OnChoosePathRequested() = 0;
    // Persist user-edited but not-yet-committed settings before the session
    // reads them from the store.
    virtual void FlushSettingsToStore() = 0;
    // The persisted recorder settings changed outside this view (toolbar
    // menu); reload from the store.
    virtual void OnRecorderSettingsChanged() = 0;
};

// Owns the lifetime of a movie-recording session — play-mode capture through
// the Game View, or a detached Player render — independently of any UI panel.
// Views observe through IMovieRecorderView registration and can be destroyed
// and rebuilt mid-recording without orphaning finalization. Reads the
// persisted recorder settings at start time; the settings UI only edits that
// store.
class MovieRecorderController
{
public:
    struct Settings
    {
        std::string Codec = "h264";
        std::string Hdr = "off"; // off | pq | hlg
        // software-ok by default: on Windows, Media Foundation "hardware" H264
        // rejects CPU-staged frames on common machines (D3D-surface-only MFTs),
        // so hardware-required is an opt-in, not a default.
        std::string EncoderMode = "software-ok";
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

    MovieRecorderController();
    ~MovieRecorderController();

    MovieRecorderController(const MovieRecorderController&) = delete;
    MovieRecorderController& operator=(const MovieRecorderController&) = delete;

    void SetPlayMode(PlayModeManager* playMode) { m_PlayMode = playMode; }
    // The primary Game View sessions record through.
    void SetGetGameView(std::function<GameViewController*()> fn) { m_GetGameView = std::move(fn); }
    // Visits every live Game View; used for cross-window sweeps (play-mode-exit
    // stop, capture frame-rate query).
    void SetForEachGameView(std::function<void(const std::function<void(GameViewController&)>&)> fn) { m_ForEachGameView = std::move(fn); }
    void SetGetCurrentScenePath(std::function<std::optional<std::filesystem::path>()> fn) { m_GetCurrentScenePath = std::move(fn); }
    // Drives the app's fixed frame rate while a capture is running so one
    // rendered frame becomes one movie frame.
    void SetFixedFrameRateHooks(std::function<void(double)> set, std::function<void()> clear)
    {
        m_SetFixedFrameRate = std::move(set);
        m_ClearFixedFrameRate = std::move(clear);
    }

    // App-level hooks (navigation/side effects outside the recorder views).
    void SetOnPlayModeRecordingStarted(std::function<void()> fn) { m_OnPlayModeRecordingStarted = std::move(fn); }
    void SetOnRecordingFinished(std::function<void(const std::filesystem::path&)> fn) { m_OnRecordingFinished = std::move(fn); }
    void SetOnOutputPathMissing(std::function<void()> fn) { m_OnOutputPathMissing = std::move(fn); }
    void RegisterSettingsCategory(
        UndoRedoService* undo,
        std::function<void(const std::filesystem::path&, const std::string&)> onPlayVideo);

    // Session views. Views must not register or unregister from inside a view
    // event.
    void RegisterView(IMovieRecorderView* view);
    void UnregisterView(IMovieRecorderView* view);
    // Session state that fired while no view was registered; a freshly built
    // view pulls these on its first tick.
    std::optional<std::filesystem::path> TakePendingFinishedRecording();
    bool TakePendingChoosePathPrompt();

    // A recording driven outside the controller (debug server) completed.
    void NotifyExternalRecordingFinished(const std::filesystem::path& path);

    // Store-backed settings shared with UI outside the settings view (toolbar
    // record menu). Setters persist to the settings store and notify
    // registered views to reload.
    std::string GetResolutionPresetValue() const;
    bool GetFadeIn() const;
    bool GetFadeOut() const;
    std::string GetFadeColor() const;
    void ApplyResolutionPreset(const std::string& value);
    void SetFadeEnabled(bool fadeIn, bool fadeOut);
    void SetFadeColor(const std::string& value);

    // Record-button semantics: stops the active session, otherwise starts one
    // from the persisted settings.
    void ToggleRecording();
    void Tick();

    bool IsCapturing() const;
    // True from the record request (armed, capturing, or a Player render in
    // flight) until the session ends — drives the toolbar record indicator so
    // feedback is immediate on click, not delayed until capture starts.
    bool IsSessionActive() const;
    const std::filesystem::path& GetActiveRecordingPath() const { return m_ActiveRecordingPath; }
    const std::filesystem::path& GetLastRecordingOutputPath() const { return m_LastRecordingOutputPath; }

    static Settings LoadSettings();
    static std::filesystem::path GetRecordingsDirectory();

private:
    GameViewController* GetGameView() const { return m_GetGameView ? m_GetGameView() : nullptr; }
    void EmitStatus(const std::string& message, bool warning = false);
    void FireRecordingStarted(const std::filesystem::path& path);
    void FireRecordingStopRequested(const std::filesystem::path& path);
    void FireRecordingFinished(const std::filesystem::path& path);
    void RequestChoosePathPrompt();
    void FlushViewSettingsToStore();
    void NotifyViewsSettingsChanged();
    void StopRecordingsOnPlayModeExit();
    void UpdateFixedFrameRate();
    bool StopActiveRecording();
    bool StartPlayModeRecording();
    void StartStandaloneRecording(const Settings& settings, const std::filesystem::path& requestedOutput);
    void ExitPlayModeAfterRecording();
    void PollStandalonePlayerExit();
    void HandleExternalStopDetection();
    void HandlePendingFinalization();
    void HandleRecordAfterPlayStarts();
    bool IsFinalizationPending() const;
    void ClearFinalizationMailbox();
    std::filesystem::path WriteMovieGameConfig(const Settings& settings);
    static std::filesystem::path ResolveRequestedOutputPath(const Settings& settings);
    static std::filesystem::path NextAvailableOutputPath(const std::filesystem::path& requested);
    static std::filesystem::path ResolvePlayerExecutable();

    PlayModeManager* m_PlayMode = nullptr;
    std::function<GameViewController*()> m_GetGameView;
    std::function<void(const std::function<void(GameViewController&)>&)> m_ForEachGameView;
    std::function<std::optional<std::filesystem::path>()> m_GetCurrentScenePath;
    std::function<void(double)> m_SetFixedFrameRate;
    std::function<void()> m_ClearFixedFrameRate;

    std::function<void(const std::filesystem::path&)> m_OnRecordingFinished;
    std::function<void()> m_OnPlayModeRecordingStarted;
    std::function<void()> m_OnOutputPathMissing;

    std::vector<IMovieRecorderView*> m_Views;
    // Session results that fired with no view alive, held for the next view.
    std::optional<std::filesystem::path> m_PendingFinishedRecording;
    bool m_PendingChoosePathPrompt = false;
    bool m_WasPlayModeActive = false;
    bool m_FixedFrameRateActive = false;

    // The Game View's status callback fires on the encoder thread and can land after
    // this controller is gone (the recorder is joined by ~GameViewController, which
    // runs later). It writes into this mailbox and holds it alive by shared_ptr, so
    // it never reaches back into the controller.
    struct FinalizationMailbox
    {
        std::mutex Mutex;
        std::string Message;
        bool Pending = false;
    };
    std::shared_ptr<FinalizationMailbox> m_Finalization = std::make_shared<FinalizationMailbox>();

    std::filesystem::path m_ActiveRecordingPath;
    std::filesystem::path m_LastRecordingOutputPath;
    std::filesystem::path m_PendingFinalizationPath;
    bool m_RecordAfterPlayStarts = false;
    bool m_ExitPlayModeWhenRecordingStops = false;
    // True from the stop request (ours or external) until finalization lands;
    // suppresses a duplicate stop event from external-stop detection.
    bool m_StopEventFired = false;
    bool m_WasCapturing = false;
    int m_StandalonePlayerPid = 0;
    std::filesystem::path m_StandalonePlayerOutputPath;
};

} // namespace Editor
} // namespace GameEngine

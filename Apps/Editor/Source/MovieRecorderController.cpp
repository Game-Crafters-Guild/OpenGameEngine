#include "MovieRecorderController.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/RenderPipelineSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Build/GameConfig.h"
#include "Engine/GameUI/UIScaleProjectSettings.h"
#include "GameViewController.h"
#include "Logger/Logger.h"
#include "Panels/MovieRecorderPanel.h"
#include "PlayMode/PlayModeManager.h"
#include "Platform/Shell.h"
#include "Rendering/Passes/TemporalDither.h"
#include "Video/VideoWriter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace GameEngine::Editor
{

namespace
{
SettingsStore OpenSettingsStore()
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    return root.empty() ? OpenEditorPreferences() : OpenProjectSettings(root);
}

std::string Trim(std::string value)
{
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
    {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t' || value[start] == '\r' || value[start] == '\n'))
    {
        ++start;
    }
    if (start > 0)
    {
        value.erase(0, start);
    }
    return value;
}

bool CodecUsesBitrate(const std::string& codec)
{
    return codec == "h265" || codec == "h264";
}

const char* CodecDefaultExtension(const std::string& codec)
{
    if (codec == "h265" || codec == "h264")
    {
        return ".mp4";
    }
    return ".mov";
}

std::string LowercaseExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

constexpr MovieRecorderResolutionPreset kResolutionPresets[] = {
    {"1080p60", "1080p 60", 1920, 1080, 60, 20000},
    {"4k60", "4K 60", 3840, 2160, 60, 80000},
    {"8k60", "8K 60", 7680, 4320, 60, 240000},
    {"1080p24", "1080p 24", 1920, 1080, 24, 12000},
    {"4k24", "4K 24", 3840, 2160, 24, 45000},
    {"8k24", "8K 24", 7680, 4320, 24, 140000},
    {"1080p25", "1080p 25", 1920, 1080, 25, 12000},
    {"4k25", "4K 25", 3840, 2160, 25, 45000},
    {"8k25", "8K 25", 7680, 4320, 25, 140000},
    {"1080p30", "1080p 30", 1920, 1080, 30, 12000},
    {"4k30", "4K 30", 3840, 2160, 30, 45000},
    {"8k30", "8K 30", 7680, 4320, 30, 140000},
};
} // namespace

std::span<const MovieRecorderResolutionPreset> GetMovieRecorderResolutionPresets()
{
    return kResolutionPresets;
}

const MovieRecorderResolutionPreset* FindMovieRecorderResolutionPreset(const std::string& value)
{
    for (const auto& preset : kResolutionPresets)
    {
        if (value == preset.Value)
        {
            return &preset;
        }
    }
    return nullptr;
}

void MovieRecorderController::RegisterSettingsCategory(
    UndoRedoService* undo,
    std::function<void(const std::filesystem::path&, const std::string&)> onPlayVideo)
{
    SettingsCategoryDescriptor recording;
    recording.CategoryId = "recording";
    recording.Title = "Recording";
    recording.Group = SettingsCategoryGroup::ProjectSettings;
    recording.TreeRowClass = "recording-row";
    recording.SearchKeywords =
        "movie recorder gameplay video h264 codec output path";
    recording.CreateContent =
        [this, undo, onPlayVideo = std::move(onPlayVideo)]() -> std::unique_ptr<UIElement>
    {
        auto panel = std::make_unique<MovieRecorderPanel>(true);
        panel->AddClass("settings-recording-panel");
        panel->SetUndoRedoService(undo);
        panel->SetOnPlayVideo(onPlayVideo);
        panel->SetController(this);
        return panel;
    };
    EditorSettingsRegistry::Get().RegisterCategory(std::move(recording));
}

std::string MovieRecorderResolutionPresetValueFor(int width, int height, int fps)
{
    for (const auto& preset : kResolutionPresets)
    {
        if (preset.Width == width && preset.Height == height && preset.Fps == fps)
        {
            return preset.Value;
        }
    }
    return "custom";
}

MovieRecorderController::MovieRecorderController() = default;

MovieRecorderController::~MovieRecorderController() = default;

bool MovieRecorderController::IsFinalizationPending() const
{
    std::lock_guard<std::mutex> lock(m_Finalization->Mutex);
    return m_Finalization->Pending;
}

void MovieRecorderController::ClearFinalizationMailbox()
{
    std::lock_guard<std::mutex> lock(m_Finalization->Mutex);
    m_Finalization->Pending = false;
    m_Finalization->Message.clear();
}

MovieRecorderController::Settings MovieRecorderController::LoadSettings()
{
    auto prefs = OpenSettingsStore();
    std::string err;
    (void)prefs.Load(&err);

    Settings settings{};
    prefs.TryGetString(MovieRecorderPrefs::kCodec, settings.Codec);
    prefs.TryGetString(MovieRecorderPrefs::kHdr, settings.Hdr);
    prefs.TryGetString(MovieRecorderPrefs::kEncoderMode, settings.EncoderMode);
    prefs.TryGetBool(MovieRecorderPrefs::kOutputPathSet, settings.OutputPathSet);
    if (settings.OutputPathSet)
    {
        prefs.TryGetString(MovieRecorderPrefs::kOutputPath, settings.OutputPath);
    }
    settings.OutputPath = Trim(settings.OutputPath);

    auto loadInt = [&prefs](const char* key, int fallback) {
        int64_t value = fallback;
        prefs.TryGetInt64(key, value);
        return static_cast<int>(value);
    };
    settings.Fps = std::max(1, loadInt(MovieRecorderPrefs::kFps, settings.Fps));
    settings.FrameCount = std::max(1, loadInt(MovieRecorderPrefs::kFrameCount, settings.FrameCount));
    settings.Width = std::clamp(loadInt(MovieRecorderPrefs::kWidth, settings.Width), 16, 16384);
    settings.Height = std::clamp(loadInt(MovieRecorderPrefs::kHeight, settings.Height), 16, 16384);
    settings.BitrateKbps = std::clamp(loadInt(MovieRecorderPrefs::kBitrateKbps, settings.BitrateKbps), 100, 1000000);
    settings.FadeDurationSeconds = std::clamp(loadInt(MovieRecorderPrefs::kFadeDurationSeconds, settings.FadeDurationSeconds), 0, 60);
    prefs.TryGetBool(MovieRecorderPrefs::kAutoBitrate, settings.AutoBitrate);
    prefs.TryGetBool(MovieRecorderPrefs::kRecordAudio, settings.RecordAudio);
    prefs.TryGetBool(MovieRecorderPrefs::kFadeIn, settings.FadeIn);
    prefs.TryGetBool(MovieRecorderPrefs::kFadeOut, settings.FadeOut);
    prefs.TryGetString(MovieRecorderPrefs::kFadeColor, settings.FadeColor);
    return settings;
}

std::filesystem::path MovieRecorderController::GetRecordingsDirectory()
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    if (!root.empty())
    {
        return root / "MovieRecordings";
    }
    return PathUtils::GetUserDataDirectory() / "GameEngine" / "MovieRecordings";
}

std::filesystem::path MovieRecorderController::ResolveRequestedOutputPath(const Settings& settings)
{
    if (settings.OutputPath.empty())
    {
        return {};
    }
    std::filesystem::path output = settings.OutputPath;
    if (output.extension().empty())
    {
        output.replace_extension(CodecDefaultExtension(settings.Codec));
    }
    return output;
}

std::filesystem::path MovieRecorderController::NextAvailableOutputPath(const std::filesystem::path& requested)
{
    if (requested.empty())
    {
        return {};
    }

    std::error_code ec;
    if (!std::filesystem::exists(requested, ec))
    {
        return requested;
    }

    const auto parent = requested.parent_path();
    const std::string stem = requested.stem().string();
    const std::string extension = requested.extension().string();
    for (int i = 1; i < 10000; ++i)
    {
        std::filesystem::path candidate = parent / (stem + " " + std::to_string(i) + extension);
        ec.clear();
        if (!std::filesystem::exists(candidate, ec))
        {
            return candidate;
        }
    }

    return parent / (stem + " " + std::to_string(
                         std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count()) + extension);
}

std::filesystem::path MovieRecorderController::ResolvePlayerExecutable()
{
    const auto exeDir = PathUtils::GetExecutableDirectory();
#if defined(_WIN32)
    const std::vector<std::filesystem::path> candidates = {
        exeDir / "Player.exe",
        exeDir / "Apps" / "Player" / "Player.exe",
        exeDir.parent_path() / "Apps" / "Player" / "Player.exe",
    };
#elif defined(__APPLE__)
    const std::vector<std::filesystem::path> candidates = {
        exeDir / "Player",
        exeDir / "Player.app" / "Contents" / "MacOS" / "Player",
        exeDir / "Apps" / "Player" / "Player",
        exeDir.parent_path() / "Apps" / "Player" / "Player",
    };
#else
    const std::vector<std::filesystem::path> candidates = {
        exeDir / "Player",
        exeDir / "Apps" / "Player" / "Player",
        exeDir.parent_path() / "Apps" / "Player" / "Player",
    };
#endif

    std::error_code ec;
    for (const auto& candidate : candidates)
    {
        if (std::filesystem::exists(candidate, ec))
        {
            return candidate;
        }
    }
    return {};
}

void MovieRecorderController::RegisterView(IMovieRecorderView* view)
{
    if (!view || std::find(m_Views.begin(), m_Views.end(), view) != m_Views.end())
    {
        return;
    }
    m_Views.push_back(view);
}

void MovieRecorderController::UnregisterView(IMovieRecorderView* view)
{
    m_Views.erase(std::remove(m_Views.begin(), m_Views.end(), view), m_Views.end());
}

std::optional<std::filesystem::path> MovieRecorderController::TakePendingFinishedRecording()
{
    auto pending = std::move(m_PendingFinishedRecording);
    m_PendingFinishedRecording.reset();
    return pending;
}

bool MovieRecorderController::TakePendingChoosePathPrompt()
{
    const bool pending = m_PendingChoosePathPrompt;
    m_PendingChoosePathPrompt = false;
    return pending;
}

void MovieRecorderController::NotifyExternalRecordingFinished(const std::filesystem::path& path)
{
    m_LastRecordingOutputPath = path;
    if (m_Views.empty())
    {
        m_PendingFinishedRecording = path;
        return;
    }
    for (auto* view : m_Views)
    {
        view->OnExternalRecordingFinished(path);
    }
}

void MovieRecorderController::EmitStatus(const std::string& message, bool warning)
{
    for (auto* view : m_Views)
    {
        view->OnRecorderStatus(message, warning);
    }
}

void MovieRecorderController::FireRecordingStarted(const std::filesystem::path& path)
{
    for (auto* view : m_Views)
    {
        view->OnRecordingStarted(path);
    }
}

void MovieRecorderController::FireRecordingStopRequested(const std::filesystem::path& path)
{
    for (auto* view : m_Views)
    {
        view->OnRecordingStopRequested(path);
    }
}

void MovieRecorderController::FireRecordingFinished(const std::filesystem::path& path)
{
    if (m_Views.empty())
    {
        // Held until a view exists; the app hook below usually opens one.
        m_PendingFinishedRecording = path;
    }
    else
    {
        for (auto* view : m_Views)
        {
            view->OnRecordingFinished(path);
        }
    }
    if (m_OnRecordingFinished)
    {
        m_OnRecordingFinished(path);
    }
}

void MovieRecorderController::RequestChoosePathPrompt()
{
    // App hook first: it reveals the Recording page, which may build and
    // register the view that then shows the prompt.
    if (m_OnOutputPathMissing)
    {
        m_OnOutputPathMissing();
    }
    if (m_Views.empty())
    {
        m_PendingChoosePathPrompt = true;
        return;
    }
    for (auto* view : m_Views)
    {
        view->OnChoosePathRequested();
    }
}

void MovieRecorderController::FlushViewSettingsToStore()
{
    for (auto* view : m_Views)
    {
        view->FlushSettingsToStore();
    }
}

void MovieRecorderController::NotifyViewsSettingsChanged()
{
    for (auto* view : m_Views)
    {
        view->OnRecorderSettingsChanged();
    }
}

std::string MovieRecorderController::GetResolutionPresetValue() const
{
    const Settings settings = LoadSettings();
    return MovieRecorderResolutionPresetValueFor(settings.Width, settings.Height, settings.Fps);
}

bool MovieRecorderController::GetFadeIn() const
{
    return LoadSettings().FadeIn;
}

bool MovieRecorderController::GetFadeOut() const
{
    return LoadSettings().FadeOut;
}

std::string MovieRecorderController::GetFadeColor() const
{
    return LoadSettings().FadeColor;
}

void MovieRecorderController::ApplyResolutionPreset(const std::string& value)
{
    const auto* preset = FindMovieRecorderResolutionPreset(value);
    if (!preset)
    {
        return;
    }

    auto prefs = OpenSettingsStore();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetInt64(MovieRecorderPrefs::kWidth, preset->Width);
    prefs.SetInt64(MovieRecorderPrefs::kHeight, preset->Height);
    prefs.SetInt64(MovieRecorderPrefs::kFps, preset->Fps);
    bool autoBitrate = true;
    prefs.TryGetBool(MovieRecorderPrefs::kAutoBitrate, autoBitrate);
    if (autoBitrate)
    {
        prefs.SetInt64(MovieRecorderPrefs::kBitrateKbps, preset->BitrateKbps);
    }
    (void)prefs.Save(&err);
    NotifyViewsSettingsChanged();
}

void MovieRecorderController::SetFadeEnabled(bool fadeIn, bool fadeOut)
{
    auto prefs = OpenSettingsStore();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetBool(MovieRecorderPrefs::kFadeIn, fadeIn);
    prefs.SetBool(MovieRecorderPrefs::kFadeOut, fadeOut);
    (void)prefs.Save(&err);
    NotifyViewsSettingsChanged();
}

void MovieRecorderController::SetFadeColor(const std::string& value)
{
    auto prefs = OpenSettingsStore();
    std::string err;
    (void)prefs.Load(&err);
    prefs.SetString(MovieRecorderPrefs::kFadeColor, value == "white" ? "white" : "black");
    (void)prefs.Save(&err);
    NotifyViewsSettingsChanged();
}

bool MovieRecorderController::IsCapturing() const
{
    GameViewController* gameView = GetGameView();
    return gameView && gameView->GetMovieCapture().IsCapturing();
}

bool MovieRecorderController::IsSessionActive() const
{
    return m_RecordAfterPlayStarts || m_StandalonePlayerPid != 0 || IsCapturing();
}

bool MovieRecorderController::StopActiveRecording()
{
    GameViewController* gameView = GetGameView();
    if (!gameView || !gameView->GetMovieCapture().IsRecording())
    {
        return false;
    }

    const auto output = m_ActiveRecordingPath;
    gameView->GetMovieCapture().StopRecording("Recording stopped.");
    m_StopEventFired = true;
    EmitStatus("Finalizing recording...");
    if (!output.empty())
    {
        FireRecordingStopRequested(output);
    }
    return true;
}

void MovieRecorderController::ToggleRecording()
{
    if (StopActiveRecording())
    {
        return;
    }

    FlushViewSettingsToStore();
    const Settings settings = LoadSettings();
    const auto requestedOutput = ResolveRequestedOutputPath(settings);
    if (requestedOutput.empty())
    {
        EmitStatus("Choose path before recording.");
        RequestChoosePathPrompt();
        Logger::Log::Warning("MovieRecorder: record requested without an output path");
        return;
    }

    if (m_PlayMode && m_PlayMode->IsPlayingOrPaused())
    {
        m_ExitPlayModeWhenRecordingStops = false;
        if (m_PlayMode->IsPaused())
        {
            m_PlayMode->TogglePause();
            EmitStatus("Resuming play mode and recording...");
        }
        if (StartPlayModeRecording())
        {
            return;
        }
    }

    if (m_RecordAfterPlayStarts)
    {
        m_RecordAfterPlayStarts = false;
        const bool enteredPlayForRecording = m_ExitPlayModeWhenRecordingStops;
        m_ExitPlayModeWhenRecordingStops = false;
        EmitStatus("Recording canceled.");
        // Play mode was only entered for this recording; leave it too.
        if (enteredPlayForRecording && m_PlayMode && m_PlayMode->IsPlayingOrPaused())
        {
            m_PlayMode->ExitPlayMode();
        }
        return;
    }

    if (m_PlayMode)
    {
        const auto state = m_PlayMode->GetState();
        if (state == PlayModeState::Edit || state == PlayModeState::EnteringPlay)
        {
            m_RecordAfterPlayStarts = true;
            m_ExitPlayModeWhenRecordingStops = true;
            EmitStatus("Starting play mode recording...");
            if (state == PlayModeState::Edit)
            {
                m_PlayMode->EnterPlayMode();
            }
            Tick();
            return;
        }
        if (state == PlayModeState::ChangeReview)
        {
            EmitStatus("Resolve play mode changes before recording.");
            return;
        }
        if (state == PlayModeState::ExitingPlay)
        {
            EmitStatus("Wait for play mode to exit before recording.");
            return;
        }
    }

    StartStandaloneRecording(settings, requestedOutput);
}

bool MovieRecorderController::StartPlayModeRecording()
{
    auto fail = [this](const std::string& message) {
        m_ExitPlayModeWhenRecordingStops = false;
        EmitStatus(message);
        return true;
    };

    GameViewController* gameView = GetGameView();
    if (!gameView)
    {
        return fail("Game View is not available for play-mode recording.");
    }

    if (gameView->GetMovieCapture().IsRecording())
    {
        return StopActiveRecording();
    }

    FlushViewSettingsToStore();
    const Settings settings = LoadSettings();
    const auto requestedOutput = ResolveRequestedOutputPath(settings);
    if (requestedOutput.empty())
    {
        EmitStatus("Choose path before recording.");
        RequestChoosePathPrompt();
        Logger::Log::Warning("MovieRecorder: play-mode record requested without an output path");
        return true;
    }

    if ((settings.Codec == "prores" || settings.Codec == "prores4444") && LowercaseExtension(requestedOutput) != ".mov")
    {
        return fail("ProRes output must be saved as a .mov file.");
    }

    const auto output = NextAvailableOutputPath(requestedOutput);
    std::error_code ec;
    if (!output.parent_path().empty())
    {
        std::filesystem::create_directories(output.parent_path(), ec);
    }
    if (ec)
    {
        return fail("Could not create output folder.");
    }

    Video::VideoWriterOptions options{};
    options.path = output.string();
    options.width = static_cast<uint32_t>(settings.Width);
    options.height = static_cast<uint32_t>(settings.Height);
    options.fps = static_cast<double>(settings.Fps);
    options.codec = Video::ParseVideoCodecName(settings.Codec, Video::VideoCodec::Auto);
    if (settings.Hdr == "pq")
        options.hdrMode = Video::VideoHdrMode::HDR10_PQ;
    else if (settings.Hdr == "hlg")
        options.hdrMode = Video::VideoHdrMode::HLG;
    if (options.hdrMode != Video::VideoHdrMode::Off)
        options.codec = Video::VideoCodec::HEVC; // HDR needs 10-bit Main10
    const bool manualBitrate = CodecUsesBitrate(settings.Codec) && !settings.AutoBitrate;
    options.bitrateKbps = manualBitrate ? static_cast<uint32_t>(settings.BitrateKbps) : 0u;
    options.requireHardwareAcceleration = settings.EncoderMode != "software-ok";
    options.recordAudio = settings.RecordAudio;
    options.fadeInEnabled = settings.FadeIn;
    options.fadeOutEnabled = settings.FadeOut;
    options.fadeColor = settings.FadeColor == "white" ? Video::FadeColor::White : Video::FadeColor::Black;
    options.fadeDurationSeconds = static_cast<double>(std::clamp(settings.FadeDurationSeconds, 1, 60));

    std::string error;
    if (!gameView->GetMovieCapture().StartRecording(options, 0, &error))
    {
        return fail(error.empty() ? "Could not start play-mode recording." : error);
    }

    // StartRecording waits out a still-finalizing previous session, so
    // its finished event can land mid-call. Deliver it before this session
    // adopts the pending-finalization state, or it would be silently dropped.
    HandlePendingFinalization();

    m_ActiveRecordingPath = output;
    m_LastRecordingOutputPath = output;
    m_PendingFinalizationPath = output;
    m_StopEventFired = false;
    m_WasCapturing = true;
    ClearFinalizationMailbox();
    gameView->GetMovieCapture().SetStatusCallback([mailbox = m_Finalization](const std::string& message) {
        std::lock_guard<std::mutex> lock(mailbox->Mutex);
        mailbox->Message = message;
        mailbox->Pending = true;
    });

    // Started fires first so the view tracks the session before the status lands.
    FireRecordingStarted(output);
    if (m_OnPlayModeRecordingStarted)
        m_OnPlayModeRecordingStarted();
    EmitStatus("Recording play mode: " + output.filename().string());
    Logger::Log::Info("MovieRecorder: play-mode recording started -> '{}'", output.string());
    return true;
}

void MovieRecorderController::StartStandaloneRecording(const Settings& settings, const std::filesystem::path& requestedOutput)
{
    if (m_StandalonePlayerPid != 0 && Platform::IsProcessRunning(m_StandalonePlayerPid))
    {
        EmitStatus("A recording is already in progress. Wait for it to complete.", true);
        return;
    }

    const auto player = ResolvePlayerExecutable();
    if (player.empty())
    {
        EmitStatus("Player executable not found. Build the Player target first.");
        Logger::Log::Error("MovieRecorder: Player executable not found near '{}'", PathUtils::GetExecutableDirectory().string());
        return;
    }

    const auto gameConfig = WriteMovieGameConfig(settings);
    if (gameConfig.empty())
    {
        return;
    }

    if (settings.Hdr != "off")
    {
        // The offline Player render isn't ported to the RenderGraph movie path
        // yet, so it can't produce the 10-bit HDR target. Steer to play-mode.
        EmitStatus("HDR recording is available in play-mode recording only.");
        return;
    }
    if ((settings.Codec == "prores" || settings.Codec == "prores4444") && LowercaseExtension(requestedOutput) != ".mov")
    {
        EmitStatus("ProRes output must be saved as a .mov file.");
        return;
    }

    const auto output = NextAvailableOutputPath(requestedOutput);
    const bool manualBitrate = CodecUsesBitrate(settings.Codec) && !settings.AutoBitrate;

    std::error_code ec;
    if (!output.parent_path().empty())
    {
        std::filesystem::create_directories(output.parent_path(), ec);
    }
    if (ec)
    {
        EmitStatus("Could not create output folder.");
        return;
    }

    std::vector<std::string> args = {
        "--config", gameConfig.string(),
        "--asset-root", EngineCore::GetInstance().GetResolvedAssetRoot().string(),
        "--write-movie", output.string(),
        "--movie-codec", settings.Codec,
        "--fixed-fps", std::to_string(settings.Fps),
        "--quit-after", std::to_string(settings.FrameCount),
    };
    if (manualBitrate)
    {
        args.push_back("--movie-bitrate");
        args.push_back(std::to_string(settings.BitrateKbps));
    }
    if (settings.EncoderMode == "hardware")
    {
        args.push_back("--require-hardware-encoder");
    }
    args.push_back(settings.RecordAudio ? "--movie-audio" : "--no-movie-audio");
    if (settings.FadeIn)
    {
        args.push_back("--movie-fade-in");
    }
    if (settings.FadeOut)
    {
        args.push_back("--movie-fade-out");
    }
    args.push_back("--movie-fade-color");
    args.push_back(settings.FadeColor == "white" ? "white" : "black");
    args.push_back("--movie-fade-duration");
    args.push_back(std::to_string(std::clamp(settings.FadeDurationSeconds, 1, 60)));
    // The experimental toggle is editor state and the Player has none, so it
    // rides the command line like every other recording option. Read from the
    // live setting rather than re-read from preferences: one source of truth
    // for what the in-editor recording and the standalone one both do.
    if (Rendering::Passes::IsTemporalMovieDitherEnabled())
    {
        args.push_back("--movie-temporal-dither");
    }

    const int pid = Platform::LaunchDetachedGetPid(player, args, player.parent_path());
    if (pid == 0)
    {
        EmitStatus("Could not launch Player.");
        return;
    }

    m_StandalonePlayerPid = pid;
    m_StandalonePlayerOutputPath = output;
    m_LastRecordingOutputPath = output;

    FireRecordingStarted(output);
    EmitStatus("Recording launched: " + output.filename().string());
    Logger::Log::Info("MovieRecorder: launched '{}' -> '{}' (pid {})", player.string(), output.string(), pid);
}

std::filesystem::path MovieRecorderController::WriteMovieGameConfig(const Settings& settings)
{
    auto& engine = EngineCore::GetInstance();
    const auto& wsRoot = engine.GetWorkspaceRoot();
    if (wsRoot.empty())
    {
        EmitStatus("Open a project before recording.");
        return {};
    }

    GameConfig cfg{};
    cfg.gameName = "Movie Recorder";
    cfg.windowWidth = static_cast<uint32_t>(settings.Width);
    cfg.windowHeight = static_cast<uint32_t>(settings.Height);
    cfg.windowMode = WindowMode::Windowed;
    cfg.vsync = false;
    cfg.uiScale = UIScaleProjectSettings::Load(wsRoot);
    cfg.renderPipeline = LoadActiveRenderPipelinePathFromProjectSettings(wsRoot).generic_string();
    if (cfg.renderPipeline.empty())
    {
        cfg.renderPipeline = "RenderPipelines/ForwardPlus.rendergraph";
    }

    if (m_GetCurrentScenePath)
    {
        if (auto scenePath = m_GetCurrentScenePath())
        {
            std::error_code ec;
            const auto rel = std::filesystem::relative(*scenePath, engine.GetResolvedAssetRoot(), ec);
            cfg.startupScene = ec ? scenePath->filename().string() : rel.generic_string();
        }
    }

    if (cfg.startupScene.empty())
    {
        EmitStatus("Save or open a scene before recording.");
        return {};
    }

    const auto configDir = wsRoot / ".gameengine" / "movie_recorder";
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    const auto configPath = configDir / "game.config";
    if (!SaveGameConfig(configPath, cfg))
    {
        EmitStatus("Could not write movie game.config.");
        return {};
    }
    return configPath;
}

void MovieRecorderController::ExitPlayModeAfterRecording()
{
    if (!m_PlayMode || !m_ExitPlayModeWhenRecordingStops)
    {
        return;
    }
    m_ExitPlayModeWhenRecordingStops = false;

    const auto state = m_PlayMode->GetState();
    if (state == PlayModeState::Play || state == PlayModeState::Paused)
    {
        m_PlayMode->ExitPlayMode();
    }
}

void MovieRecorderController::PollStandalonePlayerExit()
{
    if (m_StandalonePlayerPid == 0)
    {
        return;
    }
    if (Platform::IsProcessRunning(m_StandalonePlayerPid))
    {
        return;
    }

    const std::filesystem::path outputPath = m_StandalonePlayerOutputPath;
    m_StandalonePlayerPid = 0;
    m_StandalonePlayerOutputPath.clear();

    // The Player waits for encoder finalization before exiting, so the output
    // is complete once the process is gone.
    m_LastRecordingOutputPath = outputPath;
    EmitStatus("Recording complete: " + outputPath.filename().string());
    FireRecordingFinished(outputPath);
}

void MovieRecorderController::HandleExternalStopDetection()
{
    const bool capturing = IsCapturing();
    if (capturing)
    {
        m_WasCapturing = true;
        return;
    }
    if (!m_WasCapturing)
    {
        return;
    }
    m_WasCapturing = false;

    // The capture ended outside ToggleRecording (play-mode exit, debug
    // server). Surface the stop right away instead of waiting for encoder
    // finalization.
    if (!m_ActiveRecordingPath.empty() && !m_StopEventFired && !IsFinalizationPending())
    {
        m_StopEventFired = true;
        EmitStatus("Finalizing recording...");
        FireRecordingStopRequested(m_ActiveRecordingPath);
    }
}

void MovieRecorderController::HandlePendingFinalization()
{
    std::string message;
    {
        std::lock_guard<std::mutex> lock(m_Finalization->Mutex);
        if (!m_Finalization->Pending)
        {
            return;
        }
        m_Finalization->Pending = false;
        message = std::move(m_Finalization->Message);
        m_Finalization->Message.clear();
    }

    const auto output = m_PendingFinalizationPath;
    m_PendingFinalizationPath.clear();
    m_ActiveRecordingPath.clear();
    m_StopEventFired = false;
    m_WasCapturing = false;

    ExitPlayModeAfterRecording();
    m_LastRecordingOutputPath = output;
    EmitStatus(message.empty() ? "Recording finished." : message);
    if (!output.empty())
    {
        FireRecordingFinished(output);
    }
}

void MovieRecorderController::HandleRecordAfterPlayStarts()
{
    if (!m_RecordAfterPlayStarts || !m_PlayMode)
    {
        return;
    }

    if (m_PlayMode->IsPlayingOrPaused())
    {
        m_RecordAfterPlayStarts = false;
        if (m_PlayMode->IsPaused())
        {
            m_PlayMode->TogglePause();
        }
        StartPlayModeRecording();
        return;
    }

    const auto state = m_PlayMode->GetState();
    if (state == PlayModeState::Edit || state == PlayModeState::ChangeReview)
    {
        m_RecordAfterPlayStarts = false;
        m_ExitPlayModeWhenRecordingStops = false;
        EmitStatus(state == PlayModeState::ChangeReview
                       ? "Resolve play mode changes before recording."
                       : "Could not start play mode recording.");
    }
}

void MovieRecorderController::StopRecordingsOnPlayModeExit()
{
    const bool playingOrPaused = m_PlayMode && m_PlayMode->IsPlayingOrPaused();
    if (!playingOrPaused && m_WasPlayModeActive && m_ForEachGameView)
    {
        m_ForEachGameView([](GameViewController& gameView) {
            if (gameView.GetMovieCapture().IsRecording())
            {
                gameView.GetMovieCapture().StopRecording("Recording stopped: play mode exited.");
            }
        });
    }
    m_WasPlayModeActive = playingOrPaused;
}

void MovieRecorderController::UpdateFixedFrameRate()
{
    double capturingFps = 0.0;
    if (m_ForEachGameView)
    {
        m_ForEachGameView([&capturingFps](GameViewController& gameView) {
            if (capturingFps <= 0.0 && gameView.GetMovieCapture().IsCapturing())
            {
                capturingFps = gameView.GetMovieCapture().GetFps();
            }
        });
    }

    if (capturingFps > 0.0)
    {
        if (m_SetFixedFrameRate)
        {
            m_SetFixedFrameRate(capturingFps);
        }
        m_FixedFrameRateActive = true;
    }
    else if (m_FixedFrameRateActive)
    {
        if (m_ClearFixedFrameRate)
        {
            m_ClearFixedFrameRate();
        }
        m_FixedFrameRateActive = false;
    }
}

void MovieRecorderController::Tick()
{
    StopRecordingsOnPlayModeExit();
    PollStandalonePlayerExit();
    HandleExternalStopDetection();
    HandlePendingFinalization();
    HandleRecordAfterPlayStarts();
    for (auto* view : m_Views)
    {
        view->Tick();
    }
    UpdateFixedFrameRate();
}

} // namespace GameEngine::Editor

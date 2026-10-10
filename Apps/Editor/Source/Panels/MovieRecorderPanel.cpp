#include "Panels/MovieRecorderPanel.h"

#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Logger/Logger.h"
#include "MovieRecorderController.h"
#include "Platform/Shell.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/TextField.h"
#include "UI/UIManager.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "Input/KeyCodes.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace GameEngine
{

namespace
{
namespace Prefs = Editor::MovieRecorderPrefs;
constexpr auto kRecentRecordingListVerificationInterval = std::chrono::milliseconds(250);
constexpr double kRecentRecordingListVerificationSeconds = 15.0;

Editor::SettingsStore OpenSettings()
{
    const auto& root = EngineCore::GetInstance().GetWorkspaceRoot();
    return root.empty() ? Editor::OpenEditorPreferences() : Editor::OpenProjectSettings(root);
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

std::string FilenameForDisplay(const std::filesystem::path& path)
{
    return path.filename().string();
}

std::string MiddleTruncatedFilenameForRecentRow(const std::filesystem::path& path)
{
    const std::string filename = FilenameForDisplay(path);
    constexpr size_t kMaxRecentNameChars = 38;
    if (filename.size() <= kMaxRecentNameChars)
    {
        return filename;
    }

    constexpr size_t kEllipsisChars = 3;
    constexpr size_t kTailChars = 16;
    const size_t tailChars = std::min(kTailChars, kMaxRecentNameChars - kEllipsisChars - 1);
    const size_t headChars = kMaxRecentNameChars - tailChars - kEllipsisChars;
    return filename.substr(0, headChars) + "..." + filename.substr(filename.size() - tailChars);
}

std::string RecentRecordingKey(const std::filesystem::path& path)
{
    return path.lexically_normal().string();
}

std::string FormatBytes(std::uintmax_t bytes)
{
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    return buf;
}

bool IsMoviePath(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".mov" || ext == ".mp4" || ext == ".m4v" || ext == ".hevc" || ext == ".h265";
}

bool CodecUsesBitrate(const std::string& codec)
{
    return codec == "h265" || codec == "h264";
}

void StyleOutputField(UIElement* el)
{
    if (!el)
    {
        return;
    }
    el->AddClass("movie-recorder-output-field");
}

bool EqualSettings(const MovieRecorderPanel::SettingsSnapshot& a, const MovieRecorderPanel::SettingsSnapshot& b)
{
    return a.Codec == b.Codec &&
           a.Hdr == b.Hdr &&
           a.EncoderMode == b.EncoderMode &&
           a.OutputPath == b.OutputPath &&
           a.OutputPathSet == b.OutputPathSet &&
           a.Fps == b.Fps &&
           a.FrameCount == b.FrameCount &&
           a.Width == b.Width &&
           a.Height == b.Height &&
           a.BitrateKbps == b.BitrateKbps &&
           a.AutoBitrate == b.AutoBitrate &&
           a.RecordAudio == b.RecordAudio &&
           a.FadeIn == b.FadeIn &&
           a.FadeOut == b.FadeOut &&
           a.FadeColor == b.FadeColor &&
           a.FadeDurationSeconds == b.FadeDurationSeconds;
}

std::unique_ptr<UIElement> CreateMovieRecorderPrompt(const std::string& title,
                                                     const std::string& messageText,
                                                     const std::string& primaryLabel,
                                                     const std::string& secondaryLabel,
                                                     const std::string& cancelLabel,
                                                     bool showSecondary,
                                                     std::function<void()> onPrimary,
                                                     std::function<void()> onSecondary,
                                                     std::function<void()> onCancel,
                                                     Label** outMessage)
{
    auto prompt = std::make_unique<UIElement>();
    prompt->AddClass("modal-overlay");
    prompt->SetOverlayLayer(OverlayLayer::BlockingDialog);
    prompt->SetFocusable(true);

    std::function<void()> cancelAction = std::move(onCancel);
    prompt->RegisterEventHandler(kEventKeyDown, [cancelAction](UIEvent& e) {
        if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            if (cancelAction)
                cancelAction();
        }
    });

    auto backdrop = std::make_unique<UIElement>();
    backdrop->AddClass("modal-backdrop");

    auto window = std::make_unique<UIElement>();
    window->AddClass("modal-window");
    window->AddClass("modal-window-520");

    auto header = std::make_unique<UIElement>();
    header->AddClass("modal-header");
    auto titleLabel = std::make_unique<Label>();
    titleLabel->SetText(title);
    titleLabel->AddClass("modal-title");
    header->AddChild(std::move(titleLabel));
    window->AddChild(std::move(header));

    auto content = std::make_unique<UIElement>();
    content->AddClass("modal-content");
    auto message = std::make_unique<Label>();
    if (outMessage)
    {
        *outMessage = message.get();
    }
    message->SetText(messageText);
    message->AddClass("modal-message");
    content->AddChild(std::move(message));
    window->AddChild(std::move(content));

    auto footer = std::make_unique<UIElement>();
    footer->AddClass("modal-footer");

    auto cancel = std::make_unique<Button>();
    cancel->SetText(cancelLabel);
    cancel->AddClass("secondary");
    cancel->RegisterEventHandler(kEventButtonClick, [cancelAction](UIEvent&) {
        if (cancelAction)
        {
            cancelAction();
        }
    });
    footer->AddChild(std::move(cancel));

    auto spacer = std::make_unique<UIElement>();
    spacer->AddClass("modal-footer-spacer");
    footer->AddChild(std::move(spacer));

    if (showSecondary)
    {
        auto secondary = std::make_unique<Button>();
        secondary->SetText(secondaryLabel);
        secondary->AddClass("secondary");
        secondary->RegisterEventHandler(kEventButtonClick, [onSecondary = std::move(onSecondary)](UIEvent&) {
            if (onSecondary)
            {
                onSecondary();
            }
        });
        footer->AddChild(std::move(secondary));
    }

    auto primary = std::make_unique<Button>();
    primary->SetText(primaryLabel);
    primary->AddClass("primary");
    primary->RegisterEventHandler(kEventButtonClick, [onPrimary = std::move(onPrimary)](UIEvent&) {
        if (onPrimary)
        {
            onPrimary();
        }
    });
    footer->AddChild(std::move(primary));

    window->AddChild(std::move(footer));
    backdrop->AddChild(std::move(window));
    prompt->AddChild(std::move(backdrop));
    return prompt;
}
} // namespace

void MovieRecorderPanel::SetController(Editor::MovieRecorderController* controller)
{
    if (m_Controller == controller)
    {
        return;
    }
    if (m_Controller)
    {
        m_Controller->UnregisterView(this);
    }
    m_Controller = controller;
    if (m_Controller)
    {
        m_Controller->RegisterView(this);
    }
}

MovieRecorderPanel::MovieRecorderPanel(bool embeddedInSettings)
    : DockPanel(embeddedInSettings ? "Recording" : "Movie Recorder")
    , m_EmbeddedInSettings(embeddedInSettings)
{
    BuildUI();
    LoadSettings();
    RefreshRecentRecordings();
}

MovieRecorderPanel::~MovieRecorderPanel()
{
    if (m_Controller)
    {
        m_Controller->UnregisterView(this);
    }
    if (m_DeletePromptOverlay && m_DeletePromptOverlay->GetParent() && m_DeletePromptOverlay->GetParent() != this)
    {
        m_DeletePromptOverlay->GetParent()->RemoveChild(m_DeletePromptOverlay);
    }
    if (m_ChoosePathPromptOverlay && m_ChoosePathPromptOverlay->GetParent() && m_ChoosePathPromptOverlay->GetParent() != this)
    {
        m_ChoosePathPromptOverlay->GetParent()->RemoveChild(m_ChoosePathPromptOverlay);
    }
    if (m_LifetimeToken)
    {
        *m_LifetimeToken = false;
    }
}

void MovieRecorderPanel::RequestRefreshRecentRecordings(const std::filesystem::path& trackedOutputPath)
{
    m_RemovedRecentRecordingKeys.clear();
    if (!trackedOutputPath.empty())
    {
        m_LastRecordingOutputPath = trackedOutputPath;
        StartRecentRecordingListVerification(trackedOutputPath, kRecentRecordingListVerificationSeconds);
    }
    RefreshRecentRecordings();
}

void MovieRecorderPanel::Tick()
{
    if (m_Controller)
    {
        // Session results that landed while no view was alive (panel rebuilt
        // mid-recording, finish outside the Recording page).
        if (auto finished = m_Controller->TakePendingFinishedRecording())
        {
            OnRecordingFinished(*finished);
        }
        if (m_Controller->TakePendingChoosePathPrompt())
        {
            OnChoosePathRequested();
        }
    }
    PumpRecentRecordingsRefresh();
    PumpRecentRecordingListVerification();
    PumpRecentRecordingAutoRefresh();
    SyncRecordButtonState();
}

void MovieRecorderPanel::OnRecordingStarted(const std::filesystem::path& path)
{
    m_RemovedRecentRecordingKeys.erase(RecentRecordingKey(path));
    m_LastRecordingOutputPath = path;
    m_IsRecordingActual = true;
    SetRecordButtonActive(true);
    StartRecentRecordingAutoRefresh(path, kRecentRecordingListVerificationSeconds);
}

void MovieRecorderPanel::OnRecordingStopRequested(const std::filesystem::path& path)
{
    m_IsRecordingActual = false;
    SetRecordButtonActive(false);
    m_RemovedRecentRecordingKeys.erase(RecentRecordingKey(path));
    m_LastRecordingOutputPath = path;
    InsertRecentRecordingFromDisk(path, true);
    StartRecentRecordingAutoRefresh(path, kRecentRecordingListVerificationSeconds);
}

void MovieRecorderPanel::OnRecordingFinished(const std::filesystem::path& path)
{
    m_IsRecordingActual = false;
    SetRecordButtonActive(false);
    m_RemovedRecentRecordingKeys.erase(RecentRecordingKey(path));
    m_LastRecordingOutputPath = path;
    InsertRecentRecordingFromDisk(path, true);
    StartRecentRecordingAutoRefresh(path, kRecentRecordingListVerificationSeconds);
    // Refresh directly instead of RequestRefreshRecentRecordings: the finished
    // status message must stay visible.
    RefreshRecentRecordings();
}

void MovieRecorderPanel::OnRecorderStatus(const std::string& message, bool warning)
{
    SetStatusText(message, warning);
}

void MovieRecorderPanel::OnExternalRecordingFinished(const std::filesystem::path& path)
{
    RequestRefreshRecentRecordings(path);
}

void MovieRecorderPanel::OnRecorderSettingsChanged()
{
    LoadSettings();
}

void MovieRecorderPanel::FlushSettingsToStore()
{
    if (!CurrentOutputPath().empty())
    {
        m_OutputPathExplicit = true;
    }
    SaveSettings();
}

void MovieRecorderPanel::BuildUI()
{
    std::unique_ptr<ScrollView> panelScroll;
    if (!m_EmbeddedInSettings)
    {
        panelScroll = std::make_unique<ScrollView>();
        panelScroll->AddClass("movie-recorder-panel-scroll");
        m_PanelScroll = panelScroll.get();
    }

    auto root = std::make_unique<UIElement>();
    root->AddClass("movie-recorder-panel");
    root->AddClass("inspector-panel");
    root->AddClass("movie-recorder-inspector");
    if (m_EmbeddedInSettings)
    {
        root->AddClass("settings-recording-content");
    }

    auto section = std::make_unique<UIElement>();
    section->AddClass("inspector-section");
    section->AddClass("movie-recorder-settings-section");

    auto settings = std::make_unique<UIElement>();
    settings->AddClass("inspector-section-body");
    settings->AddClass("movie-recorder-settings");
    UIElement* settingsBody = settings.get();

    if (!m_EmbeddedInSettings)
    {
        UIElement* row = InspectorUI::AddRow(settingsBody);
        row->AddClass("movie-recorder-action-row");
        auto spacer = std::make_unique<UIElement>();
        spacer->AddClass("inspector-label-cell");
        row->AddChild(std::move(spacer));
        UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);

        auto record = std::make_unique<Button>();
        record->SetId("MovieRecorderRecordButton");
        record->SetText("Record");
        record->SetTooltip("Record movie");
        record->AddClass("movie-recorder-record-button");
        record->AddClass("small");
        record->AddClass("secondary");
        record->AddClass("icon-button");
        record->AddClass("record-icon");
        record->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            FlushSettingsToStore();
            if (m_Controller)
            {
                m_Controller->ToggleRecording();
            }
        });
        m_RecordButton = record.get();
        fieldContainer->AddChild(std::move(record));
    }

    {
        UIElement* row = InspectorUI::AddRow(settingsBody);
        row->AddClass("movie-recorder-status-row");
        m_StatusRow = row;
        auto spacer = std::make_unique<UIElement>();
        spacer->AddClass("inspector-label-cell");
        row->AddChild(std::move(spacer));
        UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
        fieldContainer->AddClass("movie-recorder-status-field");

        auto status = std::make_unique<Label>();
        status->AddClass("movie-recorder-status");
        status->SetText("");
        m_StatusLabel = status.get();
        fieldContainer->AddChild(std::move(status));
    }

    {
        UIElement* row = InspectorUI::AddRow(settingsBody);
        if (m_EmbeddedInSettings)
        {
            row->AddClass("movie-recorder-output-row");
        }
        UIElement* labelCell = nullptr;
        if (auto* label = InspectorUI::AddLabel(row, "Save to"))
        {
            label->AddClass("inspector-label-no-drag");
            labelCell = label->GetParent();
        }
        UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
        fieldContainer->AddClass("movie-recorder-output-field-container");

        auto browse = std::make_unique<Button>();
        browse->SetText("Browse");
        browse->AddClass("movie-recorder-browse-button");
        browse->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { BrowseOutputPath(); });
        m_BrowseButton = browse.get();
        if (m_EmbeddedInSettings && labelCell)
        {
            labelCell->AddChild(std::move(browse));
        }
        else
        {
            fieldContainer->AddChild(std::move(browse));
        }

        auto outputPath = std::make_unique<TextField>();
        StyleOutputField(outputPath.get());
        m_OutputPathField = outputPath.get();
        outputPath->SetOnCommit([this]() {
            m_OutputPathExplicit = !CurrentOutputPath().empty();
            RefreshOutputPathStatus();
            CommitSettingsChange("Change Movie Output Path");
        });
        fieldContainer->AddChild(std::move(outputPath));
    }

    {
        m_CodecDropdown = InspectorUI::AddDropdownRow(settingsBody, "Codec", {
            {"prores", "ProRes 422"},
            {"prores4444", "ProRes 4444"},
            {"h265", "H.265"},
            {"h264", "H.264"},
        }, 3, "Output codec");
        if (m_CodecDropdown)
        {
            m_CodecDropdown->SetOnValueChanged([this](const std::string&) {
                UpdateBitrateAvailability();
                CommitSettingsChange("Change Movie Codec");
            });
        }
    }

    {
        m_HdrDropdown = InspectorUI::AddDropdownRow(settingsBody, "HDR", {
            {"off", "Off (SDR)"},
            {"pq", "HDR10 (PQ)"},
            {"hlg", "HLG"},
        }, 0, "HDR delivery. PQ/HLG force the HEVC codec and produce a 10-bit BT.2020 file (YouTube HDR-compatible).");
        if (m_HdrDropdown)
        {
            m_HdrDropdown->SetOnValueChanged([this](const std::string& value) {
                // HDR is HEVC-only (10-bit Main10); switch the codec so the UI
                // and the eventual encode agree.
                if (value != "off" && m_CodecDropdown && m_CodecDropdown->GetSelectedValue() != "h265")
                    m_CodecDropdown->SetSelectedValue("h265");
                CommitSettingsChange("Change Movie HDR");
            });
        }
    }

    {
        std::vector<Dropdown::Option> presetOptions;
        presetOptions.push_back({"custom", "Custom"});
        for (const auto& preset : Editor::GetMovieRecorderResolutionPresets())
        {
            presetOptions.push_back({preset.Value, preset.Label});
        }

        m_ResolutionPresetDropdown = InspectorUI::AddDropdownRow(
            settingsBody, "Preset", presetOptions, 1, "Set output resolution and frame rate");
        if (m_ResolutionPresetDropdown)
        {
            m_ResolutionPresetDropdown->SetOnValueChanged([this](const std::string& value) {
                if (m_SuppressSettingUndo || value == "custom")
                {
                    return;
                }
                ApplyResolutionPreset(value);
            });
        }
    }

    {
        m_AutoBitrateToggle = InspectorDrag::AddToggleRow(
            settingsBody,
            "Auto Bitrate",
            true,
            [this](bool enabled) {
                if (m_SuppressSettingUndo)
                {
                    return;
                }
                if (enabled)
                {
                    ApplyPresetBitrateDefault();
                }
                UpdateBitrateAvailability();
                CommitSettingsChange("Change Movie Auto Bitrate");
            },
            "Use encoder/default bitrate; suggested values follow the selected preset");
    }

    {
        m_BitrateField = InspectorDrag::AddIntRowWithDrag(
            settingsBody, "Bitrate Kbps", 60000,
            [](int) {},
            [this](int) {
                if (m_AutoBitrateToggle && !m_SuppressSettingUndo)
                {
                    const bool oldSuppress = m_SuppressSettingUndo;
                    m_SuppressSettingUndo = true;
                    m_AutoBitrateToggle->SetValue(false);
                    m_SuppressSettingUndo = oldSuppress;
                }
                UpdateBitrateAvailability();
                CommitSettingsChange("Change Movie Bitrate");
            },
            60000, "Manual average video bitrate in kilobits per second");
        if (m_BitrateField)
        {
            m_BitrateField->SetRange(100, 1000000);
        }
    }

    {
        m_EncoderDropdown = InspectorUI::AddDropdownRow(settingsBody, "Encoder", {
            {"hardware", "Hardware accel"},
            {"software-ok", "Allow fallback"},
        }, 0, "Hardware acceleration mode");
        if (m_EncoderDropdown)
        {
            m_EncoderDropdown->SetOnValueChanged([this](const std::string&) { CommitSettingsChange("Change Movie Encoder Mode"); });
        }
    }

    {
        m_RecordAudioToggle = InspectorDrag::AddToggleRow(
            settingsBody,
            "Audio",
            true,
            [this](bool) { CommitSettingsChange("Change Movie Audio Recording"); },
            "Record engine audio into the movie");
    }

    {
        m_FadeInToggle = InspectorDrag::AddToggleRow(
            settingsBody,
            "Fade In",
            false,
            [this](bool) { CommitSettingsChange("Change Movie Fade In"); },
            "Fade in video and audio at the start");
    }

    {
        m_FadeOutToggle = InspectorDrag::AddToggleRow(
            settingsBody,
            "Fade Out",
            false,
            [this](bool) { CommitSettingsChange("Change Movie Fade Out"); },
            "Append a video fade at the end");
    }

    {
        m_FadeColorDropdown = InspectorUI::AddDropdownRow(settingsBody, "Fade Color", {
            {"black", "Black"},
            {"white", "White"},
        }, 0, "Fade color");
        if (m_FadeColorDropdown)
        {
            m_FadeColorDropdown->SetOnValueChanged([this](const std::string&) { CommitSettingsChange("Change Movie Fade Color"); });
        }
    }

    {
        m_FadeDurationField = InspectorDrag::AddIntRowWithDrag(
            settingsBody, "Fade Seconds", 1,
            [](int) {},
            [this](int) { CommitSettingsChange("Change Movie Fade Duration"); },
            1, "Fade duration in seconds");
        if (m_FadeDurationField)
        {
            m_FadeDurationField->SetRange(0, 60);
        }
    }

    {
        m_FpsField = InspectorDrag::AddIntRowWithDrag(
            settingsBody, "Frame Rate", 60,
            [](int) {},
            [this](int) {
                SyncResolutionPresetDropdown();
                CommitSettingsChange("Change Movie Frame Rate");
            },
            60, "Recorded frames per second");
        if (m_FpsField)
        {
            m_FpsField->SetRange(1, 240);
        }
    }

    {
        m_WidthField = InspectorDrag::AddIntRowWithDrag(
            settingsBody, "Width", 1920,
            [](int) {},
            [this](int) {
                SyncResolutionPresetDropdown();
                CommitSettingsChange("Change Movie Width");
            },
            1920, "Output width in pixels");
        if (m_WidthField)
        {
            m_WidthField->SetRange(16, 16384);
        }
    }

    {
        m_HeightField = InspectorDrag::AddIntRowWithDrag(
            settingsBody, "Height", 1080,
            [](int) {},
            [this](int) {
                SyncResolutionPresetDropdown();
                CommitSettingsChange("Change Movie Height");
            },
            1080, "Output height in pixels");
        if (m_HeightField)
        {
            m_HeightField->SetRange(16, 16384);
        }
    }

    if (m_EmbeddedInSettings)
    {
        for (const auto& child : settingsBody->GetChildren())
        {
            if (child)
            {
                child->AddClass("settings-row");
            }
        }
    }

    section->AddChild(std::move(settings));
    root->AddChild(std::move(section));

    auto header = std::make_unique<UIElement>();
    header->AddClass("movie-recorder-recent-header");
    auto title = std::make_unique<Label>();
    title->SetText("Recent Recordings");
    title->AddClass("movie-recorder-section-title");
    header->AddChild(std::move(title));

    auto refresh = std::make_unique<Button>();
    refresh->AddClass("movie-recorder-refresh-button");
    refresh->AddClass("icon-button");
    refresh->AddClass("secondary");
    refresh->AddClass("reset-reload-icon");
    refresh->SetTooltip("Refresh recent recordings");
    refresh->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        m_RemovedRecentRecordingKeys.clear();
        SetTransientRefreshStatus();
        RefreshRecentRecordings();
    });
    header->AddChild(std::move(refresh));
    root->AddChild(std::move(header));

    auto rows = std::make_unique<UIElement>();
    rows->AddClass("movie-recorder-recent-rows");
    m_RecentRows = rows.get();
    root->AddChild(std::move(rows));

    if (panelScroll)
    {
        panelScroll->AddContent(std::move(root));
        AddChild(std::move(panelScroll));
    }
    else
    {
        AddChild(std::move(root));
    }

}

void MovieRecorderPanel::LoadSettings()
{
    const bool oldSuppress = m_SuppressSettingUndo;
    m_SuppressSettingUndo = true;

    auto prefs = OpenSettings();
    std::string err;
    prefs.Load(&err);

    std::string codec = "h264";
    prefs.TryGetString(Prefs::kCodec, codec);
    if (m_CodecDropdown)
    {
        m_CodecDropdown->SetSelectedValue(codec);
    }

    std::string hdr = "off";
    prefs.TryGetString(Prefs::kHdr, hdr);
    if (m_HdrDropdown)
    {
        m_HdrDropdown->SetSelectedValue(hdr);
    }

    std::string encoderMode = "hardware";
    prefs.TryGetString(Prefs::kEncoderMode, encoderMode);
    if (m_EncoderDropdown)
    {
        m_EncoderDropdown->SetSelectedValue(encoderMode);
    }

    auto loadInt = [&prefs](const char* key, int fallback) {
        int64_t value = fallback;
        prefs.TryGetInt64(key, value);
        return static_cast<int>(value);
    };

    if (m_FpsField)
    {
        m_FpsField->SetValue(loadInt(Prefs::kFps, 60));
    }
    if (m_FrameCountField)
    {
        m_FrameCountField->SetValue(loadInt(Prefs::kFrameCount, 240));
    }
    if (m_WidthField)
    {
        m_WidthField->SetValue(loadInt(Prefs::kWidth, 1920));
    }
    if (m_HeightField)
    {
        m_HeightField->SetValue(loadInt(Prefs::kHeight, 1080));
    }
    if (m_BitrateField)
    {
        m_BitrateField->SetValue(loadInt(Prefs::kBitrateKbps, 60000));
    }
    if (m_AutoBitrateToggle)
    {
        bool autoBitrate = true;
        prefs.TryGetBool(Prefs::kAutoBitrate, autoBitrate);
        m_AutoBitrateToggle->SetValue(autoBitrate);
    }
    if (m_FadeDurationField)
    {
        m_FadeDurationField->SetValue(loadInt(Prefs::kFadeDurationSeconds, 1));
    }
    if (m_RecordAudioToggle)
    {
        bool recordAudio = true;
        prefs.TryGetBool(Prefs::kRecordAudio, recordAudio);
        m_RecordAudioToggle->SetValue(recordAudio);
    }
    if (m_FadeInToggle)
    {
        bool fadeIn = false;
        prefs.TryGetBool(Prefs::kFadeIn, fadeIn);
        m_FadeInToggle->SetValue(fadeIn);
    }
    if (m_FadeOutToggle)
    {
        bool fadeOut = false;
        prefs.TryGetBool(Prefs::kFadeOut, fadeOut);
        m_FadeOutToggle->SetValue(fadeOut);
    }
    if (m_FadeColorDropdown)
    {
        std::string fadeColor = "black";
        prefs.TryGetString(Prefs::kFadeColor, fadeColor);
        m_FadeColorDropdown->SetSelectedValue(fadeColor);
    }
    if (m_OutputPathField)
    {
        std::string outputPath;
        bool outputPathSet = false;
        prefs.TryGetBool(Prefs::kOutputPathSet, outputPathSet);
        if (outputPathSet)
        {
            prefs.TryGetString(Prefs::kOutputPath, outputPath);
        }
        outputPath = Trim(outputPath);
        m_OutputPathExplicit = outputPathSet && !outputPath.empty();
        m_OutputPathField->SetValue(m_OutputPathExplicit ? outputPath : std::string{});
    }

    SyncResolutionPresetDropdown();
    if (m_AutoBitrateToggle && m_AutoBitrateToggle->GetValue())
    {
        ApplyPresetBitrateDefault();
    }
    UpdateBitrateAvailability();
    m_SuppressSettingUndo = oldSuppress;
    m_LastCommittedSettings = CaptureSettingsSnapshot();
    RefreshOutputPathStatus();
}

void MovieRecorderPanel::SaveSettings()
{
    const SettingsSnapshot snapshot = CaptureSettingsSnapshot();
    auto prefs = OpenSettings();
    std::string err;
    prefs.Load(&err);
    prefs.SetString(Prefs::kCodec, snapshot.Codec);
    prefs.SetString(Prefs::kHdr, snapshot.Hdr);
    prefs.SetString(Prefs::kEncoderMode, snapshot.EncoderMode);
    prefs.SetString(Prefs::kOutputPath, snapshot.OutputPathSet ? snapshot.OutputPath : std::string{});
    prefs.SetBool(Prefs::kOutputPathSet, snapshot.OutputPathSet);
    prefs.SetInt64(Prefs::kFps, snapshot.Fps);
    prefs.SetInt64(Prefs::kFrameCount, snapshot.FrameCount);
    prefs.SetInt64(Prefs::kWidth, snapshot.Width);
    prefs.SetInt64(Prefs::kHeight, snapshot.Height);
    prefs.SetInt64(Prefs::kBitrateKbps, snapshot.BitrateKbps);
    prefs.SetBool(Prefs::kAutoBitrate, snapshot.AutoBitrate);
    prefs.SetBool(Prefs::kRecordAudio, snapshot.RecordAudio);
    prefs.SetBool(Prefs::kFadeIn, snapshot.FadeIn);
    prefs.SetBool(Prefs::kFadeOut, snapshot.FadeOut);
    prefs.SetString(Prefs::kFadeColor, snapshot.FadeColor);
    prefs.SetInt64(Prefs::kFadeDurationSeconds, snapshot.FadeDurationSeconds);
    prefs.Save(&err);
    m_LastCommittedSettings = snapshot;
}

void MovieRecorderPanel::CommitSettingsChange(const char* actionName)
{
    if (m_SuppressSettingUndo)
    {
        return;
    }

    const SettingsSnapshot before = m_LastCommittedSettings.value_or(CaptureSettingsSnapshot());
    const SettingsSnapshot after = CaptureSettingsSnapshot();
    if (EqualSettings(before, after))
    {
        return;
    }

    SaveSettings();

    if (!m_Undo)
    {
        return;
    }

    class MovieRecorderSettingsCommand final : public Editor::IEditorCommand
    {
    public:
        MovieRecorderSettingsCommand(std::string name, std::function<void()> undo, std::function<void()> redo)
            : m_Name(std::move(name)), m_Undo(std::move(undo)), m_Redo(std::move(redo))
        {
        }

        const char* GetName() const override { return m_Name.c_str(); }
        void Do() override
        {
            if (m_Redo)
            {
                m_Redo();
            }
        }
        void Undo() override
        {
            if (m_Undo)
            {
                m_Undo();
            }
        }
        void Redo() override
        {
            if (m_Redo)
            {
                m_Redo();
            }
        }

    private:
        std::string m_Name;
        std::function<void()> m_Undo;
        std::function<void()> m_Redo;
    };

    std::weak_ptr<bool> token = m_LifetimeToken;
    auto applySnapshot = [this, token](SettingsSnapshot snapshot) {
        if (auto alive = token.lock(); alive && *alive)
        {
            ApplySettingsSnapshot(snapshot);
        }
        else
        {
            auto prefs = OpenSettings();
            std::string err;
            prefs.Load(&err);
            prefs.SetString(Prefs::kCodec, snapshot.Codec);
            prefs.SetString(Prefs::kHdr, snapshot.Hdr);
            prefs.SetString(Prefs::kEncoderMode, snapshot.EncoderMode);
            prefs.SetString(Prefs::kOutputPath, snapshot.OutputPathSet ? snapshot.OutputPath : std::string{});
            prefs.SetBool(Prefs::kOutputPathSet, snapshot.OutputPathSet);
            prefs.SetInt64(Prefs::kFps, snapshot.Fps);
            prefs.SetInt64(Prefs::kFrameCount, snapshot.FrameCount);
            prefs.SetInt64(Prefs::kWidth, snapshot.Width);
            prefs.SetInt64(Prefs::kHeight, snapshot.Height);
            prefs.SetInt64(Prefs::kBitrateKbps, snapshot.BitrateKbps);
            prefs.SetBool(Prefs::kAutoBitrate, snapshot.AutoBitrate);
            prefs.SetBool(Prefs::kRecordAudio, snapshot.RecordAudio);
            prefs.SetBool(Prefs::kFadeIn, snapshot.FadeIn);
            prefs.SetBool(Prefs::kFadeOut, snapshot.FadeOut);
            prefs.SetString(Prefs::kFadeColor, snapshot.FadeColor);
            prefs.SetInt64(Prefs::kFadeDurationSeconds, snapshot.FadeDurationSeconds);
            prefs.Save(&err);
        }
    };

    m_Undo->CommitAlreadyApplied(std::make_unique<MovieRecorderSettingsCommand>(
        actionName ? actionName : "Change Movie Recorder Settings",
        [applySnapshot, before]() { applySnapshot(before); },
        [applySnapshot, after]() { applySnapshot(after); }));
}

MovieRecorderPanel::SettingsSnapshot MovieRecorderPanel::CaptureSettingsSnapshot() const
{
    SettingsSnapshot snapshot{};
    snapshot.Codec = SelectedCodec();
    snapshot.Hdr = m_HdrDropdown ? m_HdrDropdown->GetSelectedValue() : "off";
    snapshot.EncoderMode = SelectedEncoderMode();
    const auto outputPath = CurrentOutputPath();
    snapshot.OutputPath = outputPath.string();
    snapshot.OutputPathSet = m_OutputPathExplicit && !outputPath.empty();
    snapshot.Fps = ParsePositiveInt(m_FpsField, 60);
    snapshot.FrameCount = ParsePositiveInt(m_FrameCountField, 240);
    snapshot.Width = ParsePositiveInt(m_WidthField, 1920);
    snapshot.Height = ParsePositiveInt(m_HeightField, 1080);
    snapshot.BitrateKbps = ParseBitrateKbps(m_BitrateField, 60000);
    snapshot.AutoBitrate = m_AutoBitrateToggle ? m_AutoBitrateToggle->GetValue() : true;
    snapshot.RecordAudio = m_RecordAudioToggle && m_RecordAudioToggle->GetValue();
    snapshot.FadeIn = m_FadeInToggle && m_FadeInToggle->GetValue();
    snapshot.FadeOut = m_FadeOutToggle && m_FadeOutToggle->GetValue();
    snapshot.FadeColor = m_FadeColorDropdown ? m_FadeColorDropdown->GetSelectedValue() : "black";
    snapshot.FadeDurationSeconds = std::clamp(ParsePositiveInt(m_FadeDurationField, 1), 0, 60);
    return snapshot;
}

void MovieRecorderPanel::ApplySettingsSnapshot(const SettingsSnapshot& snapshot)
{
    m_SuppressSettingUndo = true;
    if (m_CodecDropdown)
    {
        m_CodecDropdown->SetSelectedValue(snapshot.Codec);
    }
    if (m_HdrDropdown)
    {
        m_HdrDropdown->SetSelectedValue(snapshot.Hdr);
    }
    if (m_EncoderDropdown)
    {
        m_EncoderDropdown->SetSelectedValue(snapshot.EncoderMode);
    }
    if (m_OutputPathField)
    {
        m_OutputPathField->SetValue(snapshot.OutputPathSet ? snapshot.OutputPath : std::string{});
    }
    m_OutputPathExplicit = snapshot.OutputPathSet && !snapshot.OutputPath.empty();
    if (m_FpsField)
    {
        m_FpsField->SetValue(snapshot.Fps);
    }
    if (m_FrameCountField)
    {
        m_FrameCountField->SetValue(snapshot.FrameCount);
    }
    if (m_WidthField)
    {
        m_WidthField->SetValue(snapshot.Width);
    }
    if (m_HeightField)
    {
        m_HeightField->SetValue(snapshot.Height);
    }
    if (m_BitrateField)
    {
        m_BitrateField->SetValue(std::clamp(snapshot.BitrateKbps, 100, 1000000));
    }
    if (m_AutoBitrateToggle)
    {
        m_AutoBitrateToggle->SetValue(snapshot.AutoBitrate);
    }
    if (m_RecordAudioToggle)
    {
        m_RecordAudioToggle->SetValue(snapshot.RecordAudio);
    }
    if (m_FadeInToggle)
    {
        m_FadeInToggle->SetValue(snapshot.FadeIn);
    }
    if (m_FadeOutToggle)
    {
        m_FadeOutToggle->SetValue(snapshot.FadeOut);
    }
    if (m_FadeColorDropdown)
    {
        m_FadeColorDropdown->SetSelectedValue(snapshot.FadeColor);
    }
    if (m_FadeDurationField)
    {
        m_FadeDurationField->SetValue(std::clamp(snapshot.FadeDurationSeconds, 0, 60));
    }
    SyncResolutionPresetDropdown();
    UpdateBitrateAvailability();
    m_SuppressSettingUndo = false;
    RefreshOutputPathStatus();
    SaveSettings();
    RefreshRecentRecordings();
}

void MovieRecorderPanel::ApplyResolutionPreset(const std::string& value)
{
    const auto* preset = Editor::FindMovieRecorderResolutionPreset(value);
    if (!preset)
    {
        return;
    }

    const bool oldSuppress = m_SuppressSettingUndo;
    m_SuppressSettingUndo = true;
    if (m_FpsField)
    {
        m_FpsField->SetValue(preset->Fps);
    }
    if (m_WidthField)
    {
        m_WidthField->SetValue(preset->Width);
    }
    if (m_HeightField)
    {
        m_HeightField->SetValue(preset->Height);
    }
    if (m_BitrateField && (!m_AutoBitrateToggle || m_AutoBitrateToggle->GetValue()))
    {
        m_BitrateField->SetValue(preset->BitrateKbps);
    }
    if (m_ResolutionPresetDropdown)
    {
        m_ResolutionPresetDropdown->SetSelectedValue(preset->Value);
    }
    m_SuppressSettingUndo = oldSuppress;

    CommitSettingsChange("Change Movie Resolution Preset");
}

void MovieRecorderPanel::SyncResolutionPresetDropdown()
{
    if (!m_ResolutionPresetDropdown)
    {
        return;
    }

    const std::string value = Editor::MovieRecorderResolutionPresetValueFor(
        ParsePositiveInt(m_WidthField, 1920),
        ParsePositiveInt(m_HeightField, 1080),
        ParsePositiveInt(m_FpsField, 60));

    const bool oldSuppress = m_SuppressSettingUndo;
    m_SuppressSettingUndo = true;
    m_ResolutionPresetDropdown->SetSelectedValue(value);
    m_SuppressSettingUndo = oldSuppress;

    if (m_AutoBitrateToggle && m_AutoBitrateToggle->GetValue())
    {
        ApplyPresetBitrateDefault();
    }
}

void MovieRecorderPanel::ApplyPresetBitrateDefault()
{
    if (!m_BitrateField)
    {
        return;
    }

    const auto* preset = Editor::FindMovieRecorderResolutionPreset(Editor::MovieRecorderResolutionPresetValueFor(
        ParsePositiveInt(m_WidthField, 1920),
        ParsePositiveInt(m_HeightField, 1080),
        ParsePositiveInt(m_FpsField, 60)));
    if (!preset)
    {
        return;
    }

    const bool oldSuppress = m_SuppressSettingUndo;
    m_SuppressSettingUndo = true;
    m_BitrateField->SetValue(preset->BitrateKbps);
    m_SuppressSettingUndo = oldSuppress;
}

void MovieRecorderPanel::UpdateBitrateAvailability()
{
    if (!m_BitrateField)
    {
        return;
    }

    const bool bitrateCodec = CodecUsesBitrate(SelectedCodec());
    const bool autoBitrate = !m_AutoBitrateToggle || m_AutoBitrateToggle->GetValue();
    if (m_AutoBitrateToggle)
    {
        m_AutoBitrateToggle->SetEnabled(bitrateCodec);
    }

    m_BitrateField->SetEnabled(bitrateCodec && !autoBitrate);
    if (!bitrateCodec)
    {
        m_BitrateField->SetTooltip("ProRes bitrate is determined by codec profile, resolution, and frame rate");
    }
    else if (autoBitrate)
    {
        m_BitrateField->SetTooltip("Preset suggestion shown; encoder chooses the final bitrate automatically");
    }
    else
    {
        m_BitrateField->SetTooltip("Manual average video bitrate in kilobits per second");
    }
}

void MovieRecorderPanel::RefreshRecentRecordings()
{
    PumpRecentRecordingsRefresh();

    if (m_RecentRefreshInFlight)
    {
        m_RecentRefreshQueued = true;
        return;
    }

    const auto recordingsDir = Editor::MovieRecorderController::GetRecordingsDirectory();
    const auto outputPath = CurrentOutputPath();
    const auto trackedOutputPath = m_LastRecordingOutputPath;
    const auto generation = ++m_RecentRefreshGeneration;
    m_RecentRefreshInFlight = true;
    m_RecentRefreshQueued = false;

    auto state = m_RecentRefreshState;
    std::weak_ptr<bool> token = m_LifetimeToken;
    EngineCore::GetInstance().GetJobSystem().EnqueueWork([state, token, generation, recordingsDir, outputPath, trackedOutputPath]() {
        std::vector<RecordingEntry> recordings;

        auto alreadyAdded = [&recordings](const std::filesystem::path& path) {
            const auto normalized = path.lexically_normal().string();
            return std::any_of(recordings.begin(), recordings.end(), [&normalized](const RecordingEntry& rec) {
                return rec.Path.lexically_normal().string() == normalized;
            });
        };

        auto scanDirectory = [&recordings, &alreadyAdded](const std::filesystem::path& dir) {
            std::error_code ec;
            if (dir.empty())
            {
                return;
            }
            if (!std::filesystem::is_directory(dir, ec))
            {
                return;
            }

            std::filesystem::directory_iterator it(
                dir,
                std::filesystem::directory_options::skip_permission_denied,
                ec);
            const std::filesystem::directory_iterator end;
            for (; !ec && it != end; it.increment(ec))
            {
                const auto& entry = *it;
                std::error_code entryEc;
                const auto path = entry.path();
                if (!entry.is_regular_file(entryEc) || entryEc || !IsMoviePath(path) || alreadyAdded(path))
                {
                    continue;
                }

                RecordingEntry rec{};
                rec.Path = path;
                rec.Modified = entry.last_write_time(entryEc);
                if (entryEc)
                {
                    rec.Modified = {};
                }
                entryEc.clear();
                rec.SizeBytes = entry.file_size(entryEc);
                if (entryEc)
                {
                    rec.SizeBytes = 0;
                }
                recordings.push_back(std::move(rec));
            }
        };

        std::error_code ec;
        std::filesystem::create_directories(recordingsDir, ec);
        scanDirectory(recordingsDir);

        if (!outputPath.parent_path().empty() &&
            outputPath.parent_path().lexically_normal() != recordingsDir.lexically_normal())
        {
            scanDirectory(outputPath.parent_path());
        }

        auto addRecordingPath = [&recordings, &alreadyAdded](const std::filesystem::path& path) {
            std::error_code pathEc;
            if (path.empty() ||
                !std::filesystem::exists(path, pathEc) ||
                !std::filesystem::is_regular_file(path, pathEc) ||
                !IsMoviePath(path) ||
                alreadyAdded(path))
            {
                return;
            }

            RecordingEntry rec{};
            rec.Path = path;
            rec.Modified = std::filesystem::last_write_time(path, pathEc);
            if (pathEc)
            {
                rec.Modified = {};
            }
            pathEc.clear();
            rec.SizeBytes = std::filesystem::file_size(path, pathEc);
            if (pathEc)
            {
                rec.SizeBytes = 0;
            }
            recordings.push_back(std::move(rec));
        };

        addRecordingPath(outputPath);
        addRecordingPath(trackedOutputPath);

        const auto trackedNormalized = trackedOutputPath.lexically_normal().string();
        std::sort(recordings.begin(), recordings.end(), [&trackedNormalized](const RecordingEntry& a, const RecordingEntry& b) {
            const bool aIsTracked = !trackedNormalized.empty() &&
                a.Path.lexically_normal().string() == trackedNormalized;
            const bool bIsTracked = !trackedNormalized.empty() &&
                b.Path.lexically_normal().string() == trackedNormalized;
            if (aIsTracked != bIsTracked)
            {
                return aIsTracked;
            }
            return a.Modified > b.Modified;
        });

        if (auto alive = token.lock(); !alive || !*alive)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        state->Result = std::move(recordings);
        state->Generation = generation;
        state->HasResult = true;
    }, JobSystem::JobPriority::Background);
}

void MovieRecorderPanel::PumpRecentRecordingsRefresh()
{
    if (!m_RecentRefreshState)
    {
        return;
    }

    std::vector<RecordingEntry> result;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_RecentRefreshState->mutex);
        if (!m_RecentRefreshState->HasResult ||
            m_RecentRefreshState->Generation <= m_AppliedRecentRefreshGeneration)
        {
            return;
        }

        result = std::move(m_RecentRefreshState->Result);
        generation = m_RecentRefreshState->Generation;
        m_RecentRefreshState->HasResult = false;
    }

    m_AppliedRecentRefreshGeneration = generation;
    m_RecentRefreshInFlight = false;
    // The "Refreshing..." note describes work that has now finished; drop it
    // rather than leaving a stale progress message on screen.
    if (m_StatusShowsRefreshing)
    {
        m_StatusShowsRefreshing = false;
        SetStatusText("");
        RefreshOutputPathStatus();
    }
    result.erase(std::remove_if(result.begin(), result.end(), [this](const RecordingEntry& rec) {
        return m_RemovedRecentRecordingKeys.count(RecentRecordingKey(rec.Path)) != 0;
    }), result.end());
    const bool sameRows =
        result.size() == m_RecentRecordings.size() &&
        std::equal(result.begin(), result.end(), m_RecentRecordings.begin(),
                   [](const RecordingEntry& incoming, const RecordingEntry& current)
                   {
                       return incoming.Path.lexically_normal() ==
                              current.Path.lexically_normal();
                   });

    if (sameRows)
    {
        for (size_t i = 0; i < result.size(); ++i)
        {
            const bool sizeChanged =
                result[i].SizeBytes != m_RecentRecordings[i].SizeBytes;
            m_RecentRecordings[i] = std::move(result[i]);
            if (sizeChanged && m_ActiveRecordingSizeLabel &&
                RecentRecordingKey(m_RecentRecordings[i].Path) ==
                    m_ActiveRecordingSizeKey)
            {
                m_ActiveRecordingSizeLabel->SetText(
                    FormatBytes(m_RecentRecordings[i].SizeBytes));
            }
        }
    }
    else
    {
        m_RecentRecordings = std::move(result);
    }

    if (!m_LastRecordingOutputPath.empty())
    {
        const auto normalized = m_LastRecordingOutputPath.lexically_normal().string();
        bool found = std::any_of(m_RecentRecordings.begin(), m_RecentRecordings.end(),
            [&normalized](const RecordingEntry& r) {
                return r.Path.lexically_normal().string() == normalized;
            });
        if (!found && !m_RecentListVerificationActive)
            InsertRecentRecordingFromDisk(m_LastRecordingOutputPath, true);
    }

    if (!sameRows)
    {
        RebuildRecentRows();
    }

    if (m_RecentRefreshQueued)
    {
        RefreshRecentRecordings();
    }
}

bool MovieRecorderPanel::InsertRecentRecordingFromDisk(const std::filesystem::path& path, bool allowInaccessible)
{
    if (path.empty())
        return false;

    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if ((!exists || ec) && !allowInaccessible)
        return false;

    RecordingEntry entry{};
    entry.Path = path;
    if (exists && !ec)
    {
        entry.Modified = std::filesystem::last_write_time(path, ec);
        if (ec)
        {
            entry.Modified = std::filesystem::file_time_type::clock::now();
        }
        ec.clear();
        entry.SizeBytes = std::filesystem::file_size(path, ec);
        if (ec)
        {
            entry.SizeBytes = 0;
        }
    }
    else
    {
        entry.Modified = std::filesystem::file_time_type::clock::now();
        entry.SizeBytes = 0;
    }
    const auto existing = std::find_if(
        m_RecentRecordings.begin(), m_RecentRecordings.end(),
        [&path](const RecordingEntry& r)
        {
            return r.Path.lexically_normal() == path.lexically_normal();
        });
    if (existing != m_RecentRecordings.end())
    {
        const bool alreadyFirst = existing == m_RecentRecordings.begin();
        *existing = entry;
        if (alreadyFirst)
        {
            const std::string key = RecentRecordingKey(path);
            const bool rowMarkedActive =
                m_ActiveRecordingSizeLabel && key == m_ActiveRecordingSizeKey;
            if (rowMarkedActive != IsActiveRecordingPath(path))
            {
                RebuildRecentRows();
            }
            else if (rowMarkedActive)
            {
                m_ActiveRecordingSizeLabel->SetText(
                    FormatBytes(entry.SizeBytes));
            }
            return true;
        }
        m_RecentRecordings.erase(existing);
    }
    m_RecentRecordings.insert(m_RecentRecordings.begin(), entry);
    RebuildRecentRows();
    return true;
}

void MovieRecorderPanel::StartRecentRecordingAutoRefresh(const std::filesystem::path& path, double seconds)
{
    if (path.empty())
    {
        return;
    }

    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    m_RecentAutoRefreshPath = path;
    m_LastRecordingOutputPath = path;
    m_RecentAutoRefreshNext = now + std::chrono::milliseconds(250);
    m_RecentAutoRefreshDeadline = now + std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(std::max(2.0, seconds)));
    m_RecentAutoRefreshActive = true;
    m_RecentAutoRefreshLastSize = 0;
    StartRecentRecordingListVerification(path, seconds);
}

void MovieRecorderPanel::StartRecentRecordingListVerification(const std::filesystem::path& path, double seconds)
{
    if (path.empty())
    {
        return;
    }

    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    m_RecentListVerificationPath = path;
    m_RecentListVerificationNext = now;
    m_RecentListVerificationDeadline = now + std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(std::max(kRecentRecordingListVerificationSeconds, seconds)));
    m_RecentListVerificationStartGeneration = m_AppliedRecentRefreshGeneration;
    m_RecentListVerificationActive = true;
}

bool MovieRecorderPanel::IsRecentRecordingListed(const std::filesystem::path& path) const
{
    if (m_RecentRecordings.empty())
    {
        return false;
    }

    return m_RecentRecordings.front().Path.lexically_normal() == path.lexically_normal();
}

void MovieRecorderPanel::PumpRecentRecordingListVerification()
{
    if (!m_RecentListVerificationActive)
    {
        return;
    }

    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const bool hasFreshRefresh = m_AppliedRecentRefreshGeneration > m_RecentListVerificationStartGeneration;
    if (hasFreshRefresh && IsRecentRecordingListed(m_RecentListVerificationPath))
    {
        m_RecentListVerificationActive = false;
        return;
    }

    if (now >= m_RecentListVerificationDeadline)
    {
        const auto filename = m_RecentListVerificationPath.filename().string();
        m_RecentListVerificationActive = false;
        SetStatusText("Recording saved, but recent list did not show " + filename, true);
        return;
    }

    if (now < m_RecentListVerificationNext)
    {
        return;
    }

    m_RecentListVerificationNext = now + kRecentRecordingListVerificationInterval;
    // Keep asking for directory-backed results until the exact written path is
    // present. This prevents a stale/optimistic row from hiding a missed refresh.
    RefreshRecentRecordings();
}

void MovieRecorderPanel::PumpRecentRecordingAutoRefresh()
{
    if (!m_RecentAutoRefreshActive)
    {
        return;
    }

    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    if (now < m_RecentAutoRefreshNext)
    {
        return;
    }

    // The movie file appears on disk before the encoder finishes flushing its
    // fragments, so reading the size at first sight reports 0/partial bytes. Keep
    // refreshing the row's size each poll and only stop once a non-zero size has
    // stopped growing — that marks finalization complete.
    std::error_code ec;
    const bool exists = std::filesystem::exists(m_RecentAutoRefreshPath, ec) && !ec;
    const std::uintmax_t size =
        exists ? std::filesystem::file_size(m_RecentAutoRefreshPath, ec) : 0;
    if (ec)
    {
        ec.clear();
    }

    // While this path is still being captured, neither a stable size nor the
    // deadline ends the poll: the row must keep tracking the growing file for
    // however long the user records.
    const bool stillCapturing = IsActiveRecordingPath(m_RecentAutoRefreshPath);

    if (exists)
    {
        InsertRecentRecordingFromDisk(m_RecentAutoRefreshPath);
        const bool stable = size > 0 && size == m_RecentAutoRefreshLastSize;
        m_RecentAutoRefreshLastSize = size;
        if (stable && !m_RecentListVerificationActive && !stillCapturing)
        {
            m_RecentAutoRefreshActive = false;
            return;
        }
    }

    if (now >= m_RecentAutoRefreshDeadline)
    {
        if (stillCapturing)
        {
            m_RecentAutoRefreshDeadline = now + std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(kRecentRecordingListVerificationSeconds));
        }
        else
        {
            m_RecentAutoRefreshActive = false;
            if (!exists)
            {
                RefreshRecentRecordings();
            }
            return;
        }
    }

    m_RecentAutoRefreshNext = now + std::chrono::milliseconds(250);
}

void MovieRecorderPanel::RebuildRecentRows()
{
    if (!m_RecentRows)
    {
        return;
    }
    // Rebuild can run inside event dispatch (record/stop click, delete-row
    // prompt), where RemoveChild defers and a remove-until-empty loop never
    // terminates. RemoveAllChildren touches each child exactly once.
    m_ActiveRecordingSizeLabel = nullptr;
    m_ActiveRecordingSizeKey.clear();
    m_RecentRows->RemoveAllChildren();

    if (m_RecentRecordings.empty())
    {
        auto empty = std::make_unique<Label>();
        empty->SetText("No recordings yet");
        empty->AddClass("movie-recorder-empty");
        m_RecentRows->AddChild(std::move(empty));
        return;
    }

    const size_t count = std::min<size_t>(m_RecentRecordings.size(), 12u);
    for (size_t i = 0; i < count; ++i)
    {
        const auto& rec = m_RecentRecordings[i];
        const bool isCapturingRow = IsActiveRecordingPath(rec.Path);
        auto row = std::make_unique<UIElement>();
        row->AddClass("movie-recorder-row");
        if (i == 0)
        {
            row->AddClass("most-recent");
        }

        auto actions = std::make_unique<UIElement>();
        actions->AddClass("movie-recorder-row-actions");

        auto del = std::make_unique<Button>();
        del->AddClass("movie-recorder-row-button");
        del->AddClass("movie-recorder-delete-button");
        del->SetTooltip(isCapturingRow ? "Recording in progress" : "Delete recent recording");
        del->SetEnabled(!isCapturingRow);
        const auto deletePath = rec.Path;
        del->RegisterEventHandler(kEventButtonClick, [this, deletePath](UIEvent&) { ShowDeleteRecordingPrompt(deletePath); });
        actions->AddChild(std::move(del));

        auto reveal = std::make_unique<Button>();
        reveal->AddClass("movie-recorder-row-button");
        reveal->AddClass("movie-recorder-reveal-button");
        reveal->AddClass("folder-open-icon");
        reveal->SetTooltip("Reveal in file manager");
        const auto revealPath = rec.Path;
        reveal->RegisterEventHandler(kEventButtonClick, [this, revealPath](UIEvent&) { RevealRecording(revealPath); });
        actions->AddChild(std::move(reveal));

        auto play = std::make_unique<Button>();
        play->AddClass("movie-recorder-row-button");
        play->AddClass("play-icon");
        play->SetTooltip(isCapturingRow ? "Recording in progress" : "Play recording");
        play->SetEnabled(!isCapturingRow);
        const auto playPath = rec.Path;
        play->RegisterEventHandler(kEventButtonClick, [this, playPath](UIEvent&) { PlayRecording(playPath); });
        actions->AddChild(std::move(play));
        row->AddChild(std::move(actions));

        auto name = std::make_unique<Label>();
        const std::string fullName = FilenameForDisplay(rec.Path);
        name->SetText(MiddleTruncatedFilenameForRecentRow(rec.Path));
        name->SetTooltip(fullName);
        name->AddClass("movie-recorder-row-name");
        row->AddChild(std::move(name));

        auto meta = std::make_unique<Label>();
        Label* metaPtr = meta.get();
        meta->SetText(FormatBytes(rec.SizeBytes));
        meta->AddClass("movie-recorder-row-meta");
        row->AddChild(std::move(meta));
        if (isCapturingRow)
        {
            m_ActiveRecordingSizeLabel = metaPtr;
            m_ActiveRecordingSizeKey = RecentRecordingKey(rec.Path);
        }

        m_RecentRows->AddChild(std::move(row));
    }
}

void MovieRecorderPanel::EnsureDeleteRecordingPrompt()
{
    if (m_DeletePromptOverlay)
    {
        return;
    }

    auto prompt = CreateMovieRecorderPrompt(
        "Delete recent recording?",
        "",
        "Delete File",
        "Remove Row",
        "Cancel",
        true,
        [this]() {
            const auto path = m_PendingDeletePath;
            HideDeleteRecordingPrompt();
            DeleteRecentRecordingFile(path);
        },
        [this]() {
            const auto path = m_PendingDeletePath;
            HideDeleteRecordingPrompt();
            RemoveRecentRecordingRow(path);
        },
        [this]() { HideDeleteRecordingPrompt(); },
        &m_DeletePromptMessage);

    prompt->SetId("MovieRecorderDeletePrompt");
    m_DeletePromptOverlay = prompt.get();
    if (UIManager* ui = GetOwnerManager())
    {
        if (UIElement* root = ui->GetRootElement())
        {
            root->AddChild(std::move(prompt));
            return;
        }
    }
    AddChild(std::move(prompt));
}

void MovieRecorderPanel::EnsureChoosePathPrompt()
{
    if (m_ChoosePathPromptOverlay)
    {
        return;
    }

    auto prompt = CreateMovieRecorderPrompt(
        "Choose recording path",
        "No output path is set. Choose where recordings should be saved before recording.",
        "Browse...",
        "",
        "Cancel",
        false,
        [this]() {
            HideChoosePathPrompt();
            BrowseOutputPath();
        },
        {},
        [this]() { HideChoosePathPrompt(); },
        nullptr);

    // Focus may be requested as soon as this overlay is shown. Give it a
    // stable id while detached so FocusElement never needs to mutate the live
    // tree and invalidate the popup subtree on that click.
    prompt->SetId("MovieRecorderChoosePathPrompt");
    m_ChoosePathPromptOverlay = prompt.get();
    if (UIManager* ui = GetOwnerManager())
    {
        if (UIElement* root = ui->GetRootElement())
        {
            root->AddChild(std::move(prompt));
            return;
        }
    }
    AddChild(std::move(prompt));
}

void MovieRecorderPanel::ShowDeleteRecordingPrompt(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return;
    }
    if (IsActiveRecordingPath(path))
    {
        SetStatusText(m_Controller->IsCapturing()
                          ? "Still recording - stop the recording before deleting it."
                          : "Recording is still finalizing - try again in a moment.",
                      true);
        return;
    }

    EnsureDeleteRecordingPrompt();
    m_PendingDeletePath = path;
    if (m_DeletePromptMessage)
    {
        const std::string filename = FilenameForDisplay(path);
        m_DeletePromptMessage->SetText(
            "Recording: " + (filename.empty() ? path.string() : filename) +
            ". Remove Row keeps the movie file on disk. Delete File removes it from disk and from this list.");
    }
    if (m_DeletePromptOverlay)
    {
        m_DeletePromptOverlay->AddClass("visible");
        if (UIManager* ui = GetOwnerManager())
        {
            const auto prompt = UIElement::MakeWeakRef(m_DeletePromptOverlay);
            ui->PostToUI([ui, prompt]() {
                if (UIElement* element = prompt.Get())
                    ui->FocusElement(element);
            });
        }
    }
}

void MovieRecorderPanel::HideDeleteRecordingPrompt()
{
    m_PendingDeletePath.clear();
    if (m_DeletePromptOverlay)
    {
        m_DeletePromptOverlay->RemoveClass("visible");
    }
}

void MovieRecorderPanel::OnChoosePathRequested()
{
    // A just-created panel has no owner manager yet (attach is deferred);
    // building the overlay now would parent it into the scrolled settings
    // content instead of the window root. Retry once mounted.
    if (!GetOwnerManager())
    {
        std::weak_ptr<bool> token = m_LifetimeToken;
        PostAction([this, token]() {
            if (auto alive = token.lock(); alive && *alive)
            {
                OnChoosePathRequested();
            }
        });
        return;
    }

    EnsureChoosePathPrompt();
    if (m_ChoosePathPromptOverlay)
    {
        m_ChoosePathPromptOverlay->AddClass("visible");
        if (UIManager* ui = GetOwnerManager())
        {
            const auto prompt = UIElement::MakeWeakRef(m_ChoosePathPromptOverlay);
            ui->PostToUI([ui, prompt]() {
                if (UIElement* element = prompt.Get())
                    ui->FocusElement(element);
            });
        }
    }
}

void MovieRecorderPanel::HideChoosePathPrompt()
{
    if (m_ChoosePathPromptOverlay)
    {
        m_ChoosePathPromptOverlay->RemoveClass("visible");
    }
}

void MovieRecorderPanel::RemoveRecentRecordingRow(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return;
    }

    const std::string key = RecentRecordingKey(path);
    m_RemovedRecentRecordingKeys.insert(key);
    m_RecentRecordings.erase(std::remove_if(m_RecentRecordings.begin(), m_RecentRecordings.end(), [&key](const RecordingEntry& rec) {
        return RecentRecordingKey(rec.Path) == key;
    }), m_RecentRecordings.end());
    RebuildRecentRows();

    const std::string filename = FilenameForDisplay(path);
    SetStatusText("Removed recent recording row: " + (filename.empty() ? path.string() : filename));
}

void MovieRecorderPanel::DeleteRecentRecordingFile(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return;
    }

    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    const bool stillExists = std::filesystem::exists(path, ec);
    if (ec || (!removed && stillExists))
    {
        SetStatusText("Could not delete recording file.", true);
        return;
    }

    const std::string key = RecentRecordingKey(path);
    m_RecentRecordings.erase(std::remove_if(m_RecentRecordings.begin(), m_RecentRecordings.end(), [&key](const RecordingEntry& rec) {
        return RecentRecordingKey(rec.Path) == key;
    }), m_RecentRecordings.end());
    RebuildRecentRows();

    const std::string filename = FilenameForDisplay(path);
    SetStatusText((removed ? "Deleted recording: " : "Recording file was already gone: ") +
        (filename.empty() ? path.string() : filename));
}

void MovieRecorderPanel::SetStatusText(const std::string& message, bool warning)
{
    if (!m_StatusLabel)
    {
        return;
    }

    if (warning)
    {
        m_StatusLabel->AddClass("warning");
    }
    else
    {
        m_StatusLabel->RemoveClass("warning");
    }
    m_StatusLabel->SetText(message);
}


void MovieRecorderPanel::SyncRecordButtonState()
{
    // Controller events carry the transitions, but this panel may not have
    // existed when they fired (toolbar-started recording before the Recording
    // page built). Adopt an already-running session on the first tick.
    const bool isCapturing = m_Controller && m_Controller->IsCapturing();
    if (isCapturing == m_IsRecordingActual)
    {
        return;
    }
    m_IsRecordingActual = isCapturing;
    SetRecordButtonActive(isCapturing);
    if (isCapturing)
    {
        const auto& path = m_Controller->GetActiveRecordingPath();
        if (!path.empty())
        {
            m_RemovedRecentRecordingKeys.erase(RecentRecordingKey(path));
            m_LastRecordingOutputPath = path;
            StartRecentRecordingAutoRefresh(path, kRecentRecordingListVerificationSeconds);
            SetStatusText("Recording play mode: " + path.filename().string());
        }
    }
}

void MovieRecorderPanel::SetTransientRefreshStatus()
{
    m_StatusShowsRefreshing = true;
    SetStatusText("Refreshing recent recordings...");
}

void MovieRecorderPanel::RefreshOutputPathStatus()
{
    if (!m_StatusLabel)
    {
        return;
    }

    constexpr const char* kChoosePathMessage = "Choose path before recording.";
    if (CurrentOutputPath().empty())
    {
        SetStatusText(kChoosePathMessage);
        return;
    }

    if (m_StatusLabel->GetText() == kChoosePathMessage)
    {
        SetStatusText("");
    }
}


void MovieRecorderPanel::SetRecordButtonActive(bool active)
{
    if (!m_RecordButton)
    {
        return;
    }

    if (active)
    {
        m_RecordButton->AddClass("active");
        m_RecordButton->SetText("Stop");
        m_RecordButton->SetTooltip("Stop recording");
    }
    else
    {
        m_RecordButton->RemoveClass("active");
        m_RecordButton->SetText("Record");
        m_RecordButton->SetTooltip("Record movie");
    }
}





void MovieRecorderPanel::BrowseOutputPath()
{
    auto initialPath = CurrentOutputPath();
    if (initialPath.empty())
    {
        std::string filename = "gameplay_recording";
        filename += CodecDefaultExtension();
        initialPath = Editor::MovieRecorderController::GetRecordingsDirectory() / filename;
    }

    auto output = Platform::SaveFile(initialPath, "Movie", CodecFilterPattern());
    if (output.empty())
    {
        return;
    }
    const SettingsSnapshot before = m_LastCommittedSettings.value_or(CaptureSettingsSnapshot());
    if (output.extension().empty())
    {
        output.replace_extension(CodecDefaultExtension());
    }
    if (m_OutputPathField)
    {
        m_OutputPathField->SetValueWithoutNotify(output.string());
    }
    m_OutputPathExplicit = true;
    RefreshOutputPathStatus();

    // Keep native save-panel dismissal responsive while AppKit is unwinding the
    // modal dialog, but still let the path change participate in undo.
    std::weak_ptr<bool> token = m_LifetimeToken;
    PostAction([this, token, before]() {
        if (auto alive = token.lock(); alive && *alive)
        {
            if (!m_LastCommittedSettings)
            {
                m_LastCommittedSettings = before;
            }
            CommitSettingsChange("Change Movie Output Path");
        }
    });
}

bool MovieRecorderPanel::IsActiveRecordingPath(const std::filesystem::path& path) const
{
    // Active from record start until finalization completes — the movie has
    // no container index (moov) until then, even after capture stops, so the
    // guard must span the finalize drain, not just IsCapturing().
    if (!m_Controller)
    {
        return false;
    }
    const auto& active = m_Controller->GetActiveRecordingPath();
    return !active.empty() && active.lexically_normal() == path.lexically_normal();
}

void MovieRecorderPanel::PlayRecording(const std::filesystem::path& path)
{
    // A movie mid-capture has no container index yet and decodes as black.
    if (IsActiveRecordingPath(path))
    {
        SetStatusText(m_Controller->IsCapturing()
                          ? "Still recording - stop the recording before playing it."
                          : "Recording is still finalizing - try again in a moment.",
                      true);
        return;
    }
    if (m_OnPlayVideo)
    {
        m_OnPlayVideo(path, path.filename().string());
        return;
    }
    Platform::OpenPath(path);
}

void MovieRecorderPanel::RevealRecording(const std::filesystem::path& path)
{
    Platform::ShowInFileManager(path);
}



std::filesystem::path MovieRecorderPanel::CurrentOutputPath() const
{
    if (!m_OutputPathField)
    {
        return {};
    }
    std::filesystem::path output = Trim(m_OutputPathField->GetValue());
    if (output.empty())
    {
        return {};
    }
    if (output.extension().empty())
    {
        output.replace_extension(CodecDefaultExtension());
    }
    return output;
}



std::string MovieRecorderPanel::SelectedCodec() const
{
    if (!m_CodecDropdown)
    {
        return "h264";
    }
    const std::string value = m_CodecDropdown->GetSelectedValue();
    return value.empty() ? "h264" : value;
}

std::string MovieRecorderPanel::SelectedEncoderMode() const
{
    if (!m_EncoderDropdown)
    {
        return "hardware";
    }
    const std::string value = m_EncoderDropdown->GetSelectedValue();
    return value.empty() ? "hardware" : value;
}

const char* MovieRecorderPanel::CodecFilterPattern() const
{
    const std::string codec = SelectedCodec();
    if (codec == "h265" || codec == "h264")
    {
        return "*.mp4;*.mov;*.m4v";
    }
    return "*.mov";
}

const char* MovieRecorderPanel::CodecDefaultExtension() const
{
    const std::string codec = SelectedCodec();
    if (codec == "h265" || codec == "h264")
    {
        return ".mp4";
    }
    return ".mov";
}

int MovieRecorderPanel::ParsePositiveInt(IntField* field, int fallback) const
{
    if (!field)
    {
        return fallback;
    }
    const int value = field->GetValue();
    if (value <= 0)
    {
        return fallback;
    }
    return value;
}

int MovieRecorderPanel::ParseBitrateKbps(IntField* field, int fallback) const
{
    if (!field)
    {
        return fallback;
    }
    return std::clamp(field->GetValue(), 100, 1000000);
}

} // namespace GameEngine

#include "Inspectors/AudioInspector.h"

#include "InspectorRegistry.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AudioAsset.h"
#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"

#include "Audio/AudioSystem.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"

#include "Inspectors/InspectorUIHelpers.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace GameEngine
{

namespace
{
static void AddText(UIElement* parent, const std::string& text, const char* cssClass = "inspector-text")
{
    if (!parent)
        return;
    auto label = std::make_unique<Label>();
    label->AddClass(cssClass);
    label->SetText(text);
    parent->AddChild(std::move(label));
}

static std::string GetMetaOrDefault(const AssetRegistry& reg, const std::filesystem::path& path, const char* key, const char* defValue)
{
    std::string v;
    if (reg.TryGetMetaValue(path, key, v))
        return v;
    return defValue ? std::string(defValue) : std::string();
}

static bool GetMetaBoolOrDefault(const AssetRegistry& reg, const std::filesystem::path& path, const char* key, bool defValue)
{
    std::string v;
    if (!reg.TryGetMetaValue(path, key, v))
        return defValue;
    std::string lower = ToLowerAscii(v);
    return !(lower == "0" || lower == "false" || lower == "no" || lower == "off");
}

static int GetMetaIntOrDefault(const AssetRegistry& reg, const std::filesystem::path& path, const char* key, int defValue)
{
    std::string v;
    if (!reg.TryGetMetaValue(path, key, v))
        return defValue;
    try
    {
        return std::stoi(v);
    }
    catch (...)
    {
        return defValue;
    }
}

static int FindOptionIndexByValue(const std::vector<Dropdown::Option>& options, const std::string& value, int defIndex)
{
    for (size_t i = 0; i < options.size(); ++i)
    {
        if (options[i].value == value)
            return static_cast<int>(i);
    }
    return defIndex;
}

// Map asset "audio.category" string to bus id (0=Master, 1=Music, 2=SFX, 3=UI, 4=VO, 5=Aux).
static Audio::AudioBusId CategoryToBusId(const std::string& category)
{
    if (category == "Music")
        return static_cast<Audio::AudioBusId>(1);
    if (category == "SFX")
        return static_cast<Audio::AudioBusId>(2);
    if (category == "UI")
        return static_cast<Audio::AudioBusId>(3);
    if (category == "VO")
        return static_cast<Audio::AudioBusId>(4);
    if (category == "Aux")
        return static_cast<Audio::AudioBusId>(5);
    if (category == "Default" || category.empty())
        return static_cast<Audio::AudioBusId>(2); // SFX as default
    return static_cast<Audio::AudioBusId>(2);
}
} // namespace

void RegisterAudioInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.Object)
            return;

        auto* base = static_cast<Asset*>(ctx.Object);
        auto* audio = dynamic_cast<AudioAsset*>(base);
        if (!audio)
            return;

        const GUID guid = audio->GetGUID();
        const std::filesystem::path path = audio->GetPath();

        auto container = std::make_unique<UIElement>();
        UIElement* root = container.get();
        ctx.Parent->AddChild(std::move(container));

        // Header
        {
            auto header = std::make_unique<Label>();
            header->AddClass("inspector-header");
            header->SetText("Audio: " + audio->GetName());
            root->AddChild(std::move(header));
        }

        // Basic info
        {
            std::ostringstream oss;
            oss << path.string() << "\n";
            oss << "sampleRate: " << audio->GetSampleRate() << " Hz\n";
            oss << "channels: " << static_cast<int>(audio->GetChannels()) << "\n";
            oss << "duration: " << audio->GetDuration() << " s\n";
            oss << "pcm: " << (audio->GetPCMFormat() == AudioPCMFormat::F32 ? "f32" : "none") << " (frames=" << audio->GetPCMFrameCount() << ")\n";
            oss << "encoded: " << (audio->HasEncodedData() ? ("yes (" + std::to_string(audio->GetEncodedDataSize()) + " bytes)") : "no") << "\n";

            std::error_code ec;
            const auto fileSize = std::filesystem::file_size(path, ec);
            if (!ec)
                oss << "fileSize: " << fileSize << " bytes\n";

            AddText(root, oss.str());
        }

        // Preview playback controls
        auto previewEmitter = std::make_shared<Audio::AudioEmitterHandle>(Audio::INVALID_AUDIO_EMITTER_HANDLE);
        auto previewLoop = std::make_shared<bool>(false);
        auto setAudioPreviewHandle = ctx.SetAudioPreviewHandle; // so panel can stop preview when selection changes
        const bool canPreviewAudio = []() -> bool
        {
            auto* audioSys = EngineCore::GetInstance().GetAudioSystem();
            return audioSys && audioSys->IsInitialized();
        }();

        if (!canPreviewAudio)
        {
            AddText(root,
                    "Audio preview disabled: AudioSystem is not initialized.\n"
                    "Check logs for miniaudio init failures (e.g. 'Audio: ma_engine_init failed').",
                    "inspector-text");
        }

        {
            auto loopCheck = std::make_unique<Checkbox>();
            loopCheck->SetText("Loop Preview");
            loopCheck->SetChecked(false);
            loopCheck->SetOnValueChanged([previewLoop](const bool& v)
                                         { *previewLoop = v; });
            root->AddChild(std::move(loopCheck));
        }

        auto doPlayPreview = [guid, previewEmitter, previewLoop, setAudioPreviewHandle]()
        {
            auto* audioSys = EngineCore::GetInstance().GetAudioSystem();
            if (!audioSys || !audioSys->IsInitialized())
            {
                Logger::Log::Warning("Audio: Cannot play - AudioSystem not initialized");
                return;
            }

            AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
            SharedPtr<Asset> asset = assetManager.GetAsset(guid);
            if (!asset)
            {
                Logger::Log::Warning("Audio: Asset not found for GUID={}, attempting to load...", guid.ToString());
                assetManager.LoadAssetAsync(guid, AssetLoadPriority::High);
                return;
            }

            auto* audioAsset = dynamic_cast<AudioAsset*>(asset.get());
            if (!audioAsset)
            {
                Logger::Log::Warning("Audio: Asset is not an AudioAsset for GUID={}", guid.ToString());
                return;
            }

            if (!audioAsset->IsLoaded())
            {
                Logger::Log::Info("Audio: Asset not loaded yet, loading now... (GUID={})", guid.ToString());
                if (!audioAsset->Load())
                {
                    Logger::Log::Error("Audio: Failed to load audio asset (GUID={})", guid.ToString());
                    return;
                }
            }

            if (previewEmitter->IsValid())
            {
                audioSys->Stop(*previewEmitter);
                *previewEmitter = Audio::INVALID_AUDIO_EMITTER_HANDLE;
            }

            AssetRegistry& reg = assetManager.GetRegistry();
            std::string category = GetMetaOrDefault(reg, audioAsset->GetPath(), "audio.category", "Default");
            Audio::PlayOptions opts{};
            opts.loop = *previewLoop;
            opts.spatialized = false;
            opts.bus = CategoryToBusId(category);
            Logger::Log::Info("Audio: Starting playback (GUID={}, loop={}, category={})", guid.ToString(), *previewLoop, category);
            *previewEmitter = audioSys->Play2D(guid, opts);
            if (setAudioPreviewHandle)
                setAudioPreviewHandle(*previewEmitter);
            if (!previewEmitter->IsValid())
                Logger::Log::Warning("Audio: Play2D returned invalid handle (GUID={})", guid.ToString());
        };

        {
            auto playBtn = std::make_unique<Button>();
            playBtn->SetText("Play");
            playBtn->SetDisabled(!canPreviewAudio);
            playBtn->RegisterEventHandler(kEventButtonClick, [doPlayPreview](UIEvent&) { doPlayPreview(); });
            root->AddChild(std::move(playBtn));

            auto stopBtn = std::make_unique<Button>();
            stopBtn->SetText("Stop");
            stopBtn->SetDisabled(!canPreviewAudio);
            stopBtn->RegisterEventHandler(kEventButtonClick, [previewEmitter, setAudioPreviewHandle](UIEvent&)
                                {
                                    auto* audioSys = EngineCore::GetInstance().GetAudioSystem();
                                    if (!audioSys || !audioSys->IsInitialized())
                                        return;
                                    if (previewEmitter->IsValid())
                                    {
                                        audioSys->Stop(*previewEmitter);
                                        *previewEmitter = Audio::INVALID_AUDIO_EMITTER_HANDLE;
                                        if (setAudioPreviewHandle)
                                            setAudioPreviewHandle(Audio::INVALID_AUDIO_EMITTER_HANDLE);
                                    } });
            root->AddChild(std::move(stopBtn));
        }

        bool autoPlayOnSelection = true;
        {
            Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("audio.autoPlayOnSelection", autoPlayOnSelection);
        }
        if (autoPlayOnSelection)
        {
            // Defer so AudioSystem has a chance to be ready (it may not be initialized during inspector build)
            root->PostAction([doPlayPreview]() { doPlayPreview(); });
        }

        // Runtime settings (AssetRegistry KV)
        AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
        AssetRegistry& reg = assetManager.GetRegistry();

        AddText(root, "Audio Runtime Settings (AssetRegistry KV)\nReload to apply changes.", "inspector-text");

        // LoadPolicy dropdown
        {
            auto dd = std::make_unique<Dropdown>();
            dd->AddClass("inspector-dropdown");

            std::vector<Dropdown::Option> opts = {
                {"Auto", "Auto"},
                {"DecodeToPCM", "DecodeToPCM"},
                {"Stream", "Stream"},
            };

            const std::string current = GetMetaOrDefault(reg, path, "audio.loadPolicy", "Auto");
            dd->SetOptions(opts, FindOptionIndexByValue(opts, current, 0));
            dd->SetOnValueChanged([path](const std::string& v)
                                  { EngineCore::GetInstance().GetAssetManager().GetRegistry().SetMetaValue(path, "audio.loadPolicy", v); });

            AddText(root, "loadPolicy:", "inspector-text");
            root->AddChild(std::move(dd));
        }

        // Category dropdown
        {
            auto dd = std::make_unique<Dropdown>();
            dd->AddClass("inspector-dropdown");

            std::vector<Dropdown::Option> opts = {
                {"Default", "Default"},
                {"UI", "UI"},
                {"SFX", "SFX"},
                {"Music", "Music"},
                {"VO", "VO"},
                {"Aux", "Aux"},
            };

            const std::string current = GetMetaOrDefault(reg, path, "audio.category", "Default");
            dd->SetOptions(opts, FindOptionIndexByValue(opts, current, 0));
            dd->SetOnValueChanged([path](const std::string& v)
                                  { EngineCore::GetInstance().GetAssetManager().GetRegistry().SetMetaValue(path, "audio.category", v); });

            AddText(root, "category:", "inspector-text");
            root->AddChild(std::move(dd));
        }

        // allowVirtualization
        {
            auto cb = std::make_unique<Checkbox>();
            cb->SetText("allowVirtualization");
            cb->SetChecked(GetMetaBoolOrDefault(reg, path, "audio.allowVirtualization", true));
            cb->SetOnValueChanged([path](const bool& v)
                                  { EngineCore::GetInstance().GetAssetManager().GetRegistry().SetMetaValue(path, "audio.allowVirtualization", v ? "1" : "0"); });
            root->AddChild(std::move(cb));
        }

        // preloadFrames
        {
            AddText(root, "preloadFrames:", "inspector-text");
            auto field = std::make_unique<IntField>();
            field->SetValue(GetMetaIntOrDefault(reg, path, "audio.preloadFrames", 0));
            field->SetOnValueChanged([path](const int& v)
                                     {
                                         const int clamped = (v < 0) ? 0 : v;
                                         EngineCore::GetInstance().GetAssetManager().GetRegistry().SetMetaValue(path, "audio.preloadFrames", std::to_string(clamped)); });
            root->AddChild(std::move(field));
        }
    };

    InspectorRegistry::Get().RegisterAssetInspector(AssetType::Audio, std::move(fn));
}

} // namespace GameEngine

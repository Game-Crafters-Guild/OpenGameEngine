#include "Engine/Build/GameConfig.h"
#include "Engine/GameUI/UIScaleProjectSettings.h"
#include "Logger/Logger.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>

namespace GameEngine {

using json = nlohmann::json;

static std::string NormalizeWindowModeToken(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (char& c : text)
    {
        if (c == '_' || c == ' ')
            c = '-';
    }
    return text;
}

bool TryParseWindowMode(std::string_view text, WindowMode& outMode)
{
    const std::string mode = NormalizeWindowModeToken(std::string(text));
    if (mode == "windowed")
    {
        outMode = WindowMode::Windowed;
        return true;
    }
    if (mode == "borderless" || mode == "borderless-fullscreen" || mode == "fullscreen")
    {
        outMode = WindowMode::BorderlessFullscreen;
        return true;
    }
    if (mode == "exclusive" || mode == "exclusive-fullscreen")
    {
        outMode = WindowMode::ExclusiveFullscreen;
        return true;
    }
    return false;
}

const char* WindowModeToString(WindowMode mode)
{
    switch (mode)
    {
    case WindowMode::BorderlessFullscreen: return "borderless";
    case WindowMode::ExclusiveFullscreen:  return "exclusive";
    default:                               return "windowed";
    }
}

static Rendering::HdrSwapchainBitDepth ParseHdrSwapchainBitDepth(const json& value)
{
    if (value.is_number_integer())
        return value.get<int>() == 16 ? Rendering::HdrSwapchainBitDepth::Float16 : Rendering::HdrSwapchainBitDepth::Bit10;
    if (value.is_string())
    {
        std::string text = value.get<std::string>();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        text.erase(std::remove_if(text.begin(), text.end(), [](char c) {
            return c == '-' || c == '_' || c == ' ';
        }), text.end());
        if (text == "16" || text == "float16" || text == "fp16" || text == "rgba16f")
            return Rendering::HdrSwapchainBitDepth::Float16;
    }
    return Rendering::HdrSwapchainBitDepth::Bit10;
}

GameConfig LoadGameConfig(const std::filesystem::path& path)
{
    GameConfig cfg;

    std::ifstream file(path);
    if (!file.is_open())
    {
        Logger::Log::Warning("GameConfig: Could not open '{}', using defaults", path.string());
        return cfg;
    }

    json doc;
    try
    {
        doc = json::parse(file);
    }
    catch (const json::parse_error& e)
    {
        Logger::Log::Error("GameConfig: Parse error in '{}': {}", path.string(), e.what());
        return cfg;
    }

    if (doc.contains("gameName") && doc["gameName"].is_string())
        cfg.gameName = doc["gameName"].get<std::string>();

    if (doc.contains("startupScene") && doc["startupScene"].is_string())
        cfg.startupScene = doc["startupScene"].get<std::string>();

    if (doc.contains("renderPipeline") && doc["renderPipeline"].is_string())
        cfg.renderPipeline = doc["renderPipeline"].get<std::string>();

    if (doc.contains("scriptAssemblyPath") && doc["scriptAssemblyPath"].is_string())
        cfg.scriptAssemblyPath = doc["scriptAssemblyPath"].get<std::string>();

    if (doc.contains("appIcon") && doc["appIcon"].is_string())
        cfg.appIconPath = doc["appIcon"].get<std::string>();

    if (const auto uiScale = doc.find(UIScaleProjectSettings::kUIScaleKey); uiScale != doc.end())
        cfg.uiScale = UIScaleProjectSettings::ReadFrom(*uiScale);

    if (doc.contains("window") && doc["window"].is_object())
    {
        const auto& w = doc["window"];
        if (w.contains("width") && w["width"].is_number_unsigned())
            cfg.windowWidth = w["width"].get<uint32_t>();
        if (w.contains("height") && w["height"].is_number_unsigned())
            cfg.windowHeight = w["height"].get<uint32_t>();
        if (w.contains("mode") && w["mode"].is_string())
        {
            WindowMode parsed = WindowMode::Windowed;
            if (TryParseWindowMode(w["mode"].get<std::string>(), parsed))
                cfg.windowMode = parsed;
            else
                Logger::Log::Warning("GameConfig: Unknown window mode '{}', using windowed", w["mode"].get<std::string>());
        }
        if (w.contains("vsync") && w["vsync"].is_boolean())
            cfg.vsync = w["vsync"].get<bool>();
    }

    if (doc.contains("hdr") && doc["hdr"].is_object())
    {
        const auto& h = doc["hdr"];
        if (h.contains("enabled") && h["enabled"].is_boolean())
            cfg.hdrEnabled = h["enabled"].get<bool>();
        if (h.contains("mode") && h["mode"].is_string())
            cfg.hdrMode = Rendering::HdrOutputModeFromString(h["mode"].get<std::string>());
        if (h.contains("swapchainBitDepth"))
            cfg.hdrSwapchainBitDepth = ParseHdrSwapchainBitDepth(h["swapchainBitDepth"]);
        if (h.contains("targetDisplay") && h["targetDisplay"].is_number_integer())
            cfg.hdrTargetDisplay = h["targetDisplay"].get<int>();
        if (h.contains("metadata") && h["metadata"].is_object())
        {
            const auto& m = h["metadata"];
            auto readFloat = [&m](const char* key, float& out) {
                if (m.contains(key) && m[key].is_number())
                    out = m[key].get<float>();
            };
            readFloat("maxMasteringLuminance", cfg.hdrStaticMetadata.maxMasteringLuminance);
            readFloat("minMasteringLuminance", cfg.hdrStaticMetadata.minMasteringLuminance);
            readFloat("maxContentLightLevel", cfg.hdrStaticMetadata.maxContentLightLevel);
            readFloat("maxFrameAverageLightLevel", cfg.hdrStaticMetadata.maxFrameAverageLightLevel);
            readFloat("paperWhiteNits", cfg.hdrStaticMetadata.paperWhiteNits);
        }
    }

    if (doc.contains("lod") && doc["lod"].is_object())
    {
        const auto& l = doc["lod"];
        if (l.contains("autoGenerate") && l["autoGenerate"].is_boolean())
            cfg.lod.autoGenerate = l["autoGenerate"].get<bool>();
        if (l.contains("count") && l["count"].is_number_unsigned())
            cfg.lod.count = std::clamp<uint32_t>(l["count"].get<uint32_t>(), 1u, 4u);
        if (l.contains("borderRule") && l["borderRule"].is_number_unsigned())
        {
            // Reject out-of-range values instead of clamping: clamping mapped
            // garbage to Free (the least-safe rule); an unknown value keeps
            // the SeamPlanes default, matching LodAssetSettings::Load.
            const uint32_t rule = l["borderRule"].get<uint32_t>();
            if (rule <= 2u)
                cfg.lod.borderRule = rule;
        }
        auto readFloatArray = [](const json& arr, float* out, size_t n) {
            if (!arr.is_array()) return;
            for (size_t i = 0; i < n && i < arr.size(); ++i)
                if (arr[i].is_number()) out[i] = arr[i].get<float>();
        };
        if (l.contains("ratios")) readFloatArray(l["ratios"], cfg.lod.ratios, 4);
        if (l.contains("errors")) readFloatArray(l["errors"], cfg.lod.errors, 4);
    }

    // The same "rendering" object, with the same keys, that the project this
    // was cooked from stores in .Editor/ProjectSettings.json —
    // Rendering::LodProjectSettings owns both ends, so the two cannot drift.
    if (doc.contains("rendering"))
    {
        cfg.lodSelection = Rendering::LodProjectSettings::ReadFrom(doc["rendering"]);
        cfg.renderQuality = Rendering::AntiAliasingProjectSettings::ReadFrom(doc["rendering"]);
    }

    // Clamp window dimensions to reasonable bounds.
    constexpr uint32_t kMinWindowSize = 320;
    constexpr uint32_t kMaxWindowSize = 7680; // 8K
    cfg.windowWidth = std::clamp(cfg.windowWidth, kMinWindowSize, kMaxWindowSize);
    cfg.windowHeight = std::clamp(cfg.windowHeight, kMinWindowSize, kMaxWindowSize);

    Logger::Log::Info("GameConfig: Loaded '{}' (game='{}', scene='{}', pipeline='{}')",
                      path.string(), cfg.gameName, cfg.startupScene, cfg.renderPipeline);
    return cfg;
}

bool SaveGameConfig(const std::filesystem::path& path, const GameConfig& cfg)
{
    json doc = json::object();
    doc["gameName"] = cfg.gameName;
    UIScaleProjectSettings::WriteTo(cfg.uiScale, doc[UIScaleProjectSettings::kUIScaleKey]);
    doc["startupScene"] = cfg.startupScene;
    doc["renderPipeline"] = cfg.renderPipeline;
    doc["scriptAssemblyPath"] = cfg.scriptAssemblyPath;

    if (!cfg.appIconPath.empty())
        doc["appIcon"] = cfg.appIconPath;

    json window = json::object();
    window["width"] = cfg.windowWidth;
    window["height"] = cfg.windowHeight;
    window["mode"] = WindowModeToString(cfg.windowMode);
    window["vsync"] = cfg.vsync;
    doc["window"] = window;

    json hdr = json::object();
    hdr["enabled"] = cfg.hdrEnabled;
    hdr["mode"] = Rendering::HdrOutputModeToString(cfg.hdrMode);
    hdr["swapchainBitDepth"] = cfg.hdrSwapchainBitDepth == Rendering::HdrSwapchainBitDepth::Float16 ? 16 : 10;
    hdr["targetDisplay"] = cfg.hdrTargetDisplay;
    json metadata = json::object();
    metadata["redPrimary"] = cfg.hdrStaticMetadata.redPrimary;
    metadata["greenPrimary"] = cfg.hdrStaticMetadata.greenPrimary;
    metadata["bluePrimary"] = cfg.hdrStaticMetadata.bluePrimary;
    metadata["whitePoint"] = cfg.hdrStaticMetadata.whitePoint;
    metadata["maxMasteringLuminance"] = cfg.hdrStaticMetadata.maxMasteringLuminance;
    metadata["minMasteringLuminance"] = cfg.hdrStaticMetadata.minMasteringLuminance;
    metadata["maxContentLightLevel"] = cfg.hdrStaticMetadata.maxContentLightLevel;
    metadata["maxFrameAverageLightLevel"] = cfg.hdrStaticMetadata.maxFrameAverageLightLevel;
    metadata["paperWhiteNits"] = cfg.hdrStaticMetadata.paperWhiteNits;
    hdr["metadata"] = metadata;
    doc["hdr"] = hdr;

    json lod = json::object();
    lod["autoGenerate"] = cfg.lod.autoGenerate;
    lod["count"] = cfg.lod.count;
    lod["ratios"] = {cfg.lod.ratios[0], cfg.lod.ratios[1], cfg.lod.ratios[2], cfg.lod.ratios[3]};
    lod["errors"] = {cfg.lod.errors[0], cfg.lod.errors[1], cfg.lod.errors[2], cfg.lod.errors[3]};
    lod["borderRule"] = cfg.lod.borderRule;
    doc["lod"] = lod;

    json rendering = json::object();
    cfg.lodSelection.WriteTo(rendering);
    cfg.renderQuality.WriteTo(rendering);
    doc["rendering"] = rendering;

    std::ofstream file(path);
    if (!file.is_open())
    {
        Logger::Log::Error("GameConfig: Could not write '{}'", path.string());
        return false;
    }

    file << doc.dump(4) << '\n';
    return file.good();
}

} // namespace GameEngine

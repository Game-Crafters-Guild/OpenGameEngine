#include "Assets/LodAssetSettings.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Types/FormatNumber.h"

#include <algorithm>
#include <charconv>
#include <locale>
#include <sstream>

namespace GameEngine {

namespace {

bool ParseBool(const std::string& value) {
    return !(value == "0" || value == "false" || value == "False");
}

std::string_view Trim(std::string_view s) {
    const auto notSpace = [](unsigned char c) { return c != ' ' && c != '\t'; };
    size_t begin = 0;
    while (begin < s.size() && !notSpace(static_cast<unsigned char>(s[begin]))) ++begin;
    size_t end = s.size();
    while (end > begin && !notSpace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

bool ParseFloat(std::string_view value, float& out) {
    std::istringstream stream{std::string(value)};
    stream.imbue(std::locale::classic());

    float parsed = 0.0f;
    char extra = '\0';
    if (!(stream >> parsed) || (stream >> extra))
        return false;

    out = parsed;
    return true;
}

} // namespace

MeshLODConfig LodAssetSettings::ToConfig() const {
    MeshLODConfig config;
    config.LodCount = std::clamp<uint32>(LodCount, 1u, MeshLODConfig::kMaxLODs);
    for (uint32 i = 0; i < 4u; ++i) {
        config.TargetRatios[i] = TargetRatios[i];
        config.TargetError[i]  = TargetError[i];
    }
    config.BorderRule = BorderRule;
    return config;
}

std::string LodAssetSettings::EncodeFloatCsv(const float* values, uint32 count) {
    std::string out;
    for (uint32 i = 0; i < count; ++i) {
        if (i != 0) out.push_back(',');
        AppendFloat(out, values[i]);
    }
    return out;
}

void LodAssetSettings::DecodeFloatCsv(std::string_view csv, float* out, uint32 maxCount) {
    uint32 index = 0;
    size_t pos = 0;
    while (index < maxCount && pos <= csv.size()) {
        const size_t comma = csv.find(',', pos);
        const size_t tokenEnd = (comma == std::string_view::npos) ? csv.size() : comma;
        const std::string_view token = Trim(csv.substr(pos, tokenEnd - pos));
        if (!token.empty()) {
            float parsed = 0.0f;
            if (ParseFloat(token, parsed))
                out[index] = parsed; // else leave the caller's default
        }
        ++index;
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
}

LodAssetSettings LodAssetSettings::Load(const AssetRegistry& registry,
                                        const std::filesystem::path& assetPath) {
    LodAssetSettings settings;
    if (assetPath.empty())
        return settings;

    std::string value;
    if (registry.TryGetMetaValue(assetPath, kUseGlobalKey, value))
        settings.UseGlobal = ParseBool(value);
    if (registry.TryGetMetaValue(assetPath, kGenerateKey, value))
        settings.Generate = ParseBool(value);
    if (registry.TryGetMetaValue(assetPath, kCountKey, value)) {
        uint32 count = settings.LodCount;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), count);
        if (result.ec == std::errc{})
            settings.LodCount = std::clamp<uint32>(count, 1u, MeshLODConfig::kMaxLODs);
    }
    if (registry.TryGetMetaValue(assetPath, kRatiosKey, value))
        DecodeFloatCsv(value, settings.TargetRatios, 4u);
    if (registry.TryGetMetaValue(assetPath, kErrorsKey, value))
        DecodeFloatCsv(value, settings.TargetError, 4u);
    if (registry.TryGetMetaValue(assetPath, kBorderRuleKey, value)) {
        uint32 rule = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), rule);
        if (result.ec == std::errc{} && rule <= static_cast<uint32>(MeshLODBorderRule::Free))
            settings.BorderRule = static_cast<MeshLODBorderRule>(rule);
    }
    if (registry.TryGetMetaValue(assetPath, kSkinnedKey, value))
        settings.GenerateSkinned = ParseBool(value);
    return settings;
}

bool LodAssetSettings::Save(AssetRegistry& registry,
                            const std::filesystem::path& assetPath) const {
    if (assetPath.empty())
        return false;
    bool ok = true;
    ok &= registry.SetMetaValue(assetPath, kUseGlobalKey, UseGlobal ? "1" : "0");
    ok &= registry.SetMetaValue(assetPath, kGenerateKey, Generate ? "1" : "0");
    ok &= registry.SetMetaValue(assetPath, kCountKey,
                                std::to_string(std::clamp<uint32>(LodCount, 1u, MeshLODConfig::kMaxLODs)));
    ok &= registry.SetMetaValue(assetPath, kRatiosKey, EncodeFloatCsv(TargetRatios, 4u));
    ok &= registry.SetMetaValue(assetPath, kErrorsKey, EncodeFloatCsv(TargetError, 4u));
    ok &= registry.SetMetaValue(assetPath, kBorderRuleKey,
                                std::to_string(static_cast<uint32>(BorderRule)));
    ok &= registry.SetMetaValue(assetPath, kSkinnedKey, GenerateSkinned ? "1" : "0");
    return ok;
}

ResolvedLodSettings ResolveLodSettings(const LodAssetSettings& perAsset,
                                       const LODImportSettings& global) {
    ResolvedLodSettings resolved;
    if (perAsset.UseGlobal) {
        resolved.Generate = global.AutoGenerateOnImport;
        resolved.Config = global.Config;
        resolved.GenerateSkinned = false; // global tier has no skinned opt-in
    } else {
        resolved.Generate = perAsset.Generate;
        resolved.Config = perAsset.ToConfig();
        resolved.GenerateSkinned = perAsset.GenerateSkinned;
    }
    return resolved;
}

ResolvedLodSettings ResolveLodSettings(AssetManager* assetManager,
                                       const std::filesystem::path& assetPath) {
    const LODImportSettings& global = GetLODImportSettings();
    if (!assetManager || assetPath.empty())
        return ResolveLodSettings(LodAssetSettings{}, global);
    const LodAssetSettings perAsset =
        LodAssetSettings::Load(assetManager->GetRegistry(), assetPath);
    return ResolveLodSettings(perAsset, global);
}

} // namespace GameEngine

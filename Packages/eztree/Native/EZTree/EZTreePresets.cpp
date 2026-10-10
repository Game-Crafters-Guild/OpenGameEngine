// Native C++ preset loader for @dgreenheck/ez-tree JSON presets.
// Portions adapted from EZ-Tree, MIT License.
// Copyright (c) 2024 Daniel Greenheck.

#include "EZTree/EZTreeOptions.h"

#include "Core/Application.h" // PathUtils::GetExecutableDirectory

#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace GameEngine::EZTree
{
namespace
{

const std::array<PresetInfo, 16> kPresets{{
    {"ash_large", "Ash Large"},
    {"ash_medium", "Ash Medium"},
    {"ash_small", "Ash Small"},
    {"aspen_large", "Aspen Large"},
    {"aspen_medium", "Aspen Medium"},
    {"aspen_small", "Aspen Small"},
    {"bush_1", "Bush 1"},
    {"bush_2", "Bush 2"},
    {"bush_3", "Bush 3"},
    {"oak_large", "Oak Large"},
    {"oak_medium", "Oak Medium"},
    {"oak_small", "Oak Small"},
    {"pine_large", "Pine Large"},
    {"pine_medium", "Pine Medium"},
    {"pine_small", "Pine Small"},
    {"trellis", "Trellis"},
}};

bool IsKnownPreset(std::string_view name)
{
    for (const auto& preset : kPresets)
    {
        if (preset.name == name)
            return true;
    }
    return false;
}

template <typename T>
void AssignIfPresent(const nlohmann::json& j, const char* key, T& out)
{
    if (auto it = j.find(key); it != j.end() && !it->is_null())
        out = it->get<T>();
}

void AssignColorIfPresent(const nlohmann::json& j, const char* key, uint32& out)
{
    if (auto it = j.find(key); it != j.end() && it->is_number_unsigned())
        out = static_cast<uint32>(it->get<uint64_t>());
    else if (it != j.end() && it->is_number_integer())
        out = static_cast<uint32>(it->get<int64_t>());
}

template <typename T, size_t N>
void AssignMapArray(const nlohmann::json& j, const char* key, std::array<T, N>& out)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_object())
        return;
    for (size_t i = 0; i < N; ++i)
    {
        const std::string idx = std::to_string(i);
        if (auto item = it->find(idx); item != it->end() && !item->is_null())
            out[i] = item->get<T>();
    }
}

Mathematics::Vector3 ReadVec3(const nlohmann::json& j, const Mathematics::Vector3& fallback)
{
    Mathematics::Vector3 out = fallback;
    AssignIfPresent(j, "x", out.x);
    AssignIfPresent(j, "y", out.y);
    AssignIfPresent(j, "z", out.z);
    return out;
}

void ApplyPresetJson(const nlohmann::json& json, TreeOptions& out)
{
    AssignIfPresent(json, "seed", out.seed);
    if (auto it = json.find("type"); it != json.end() && it->is_string())
        out.type = TreeTypeFromString(it->get<std::string>(), out.type);

    if (auto bark = json.find("bark"); bark != json.end() && bark->is_object())
    {
        if (auto it = bark->find("type"); it != bark->end() && it->is_string())
            out.bark.type = BarkTypeFromString(it->get<std::string>(), out.bark.type);
        AssignColorIfPresent(*bark, "tint", out.bark.tint);
        AssignIfPresent(*bark, "flatShading", out.bark.flatShading);
        AssignIfPresent(*bark, "textured", out.bark.textured);
        if (auto scale = bark->find("textureScale"); scale != bark->end() && scale->is_object())
        {
            AssignIfPresent(*scale, "x", out.bark.textureScale.x);
            AssignIfPresent(*scale, "y", out.bark.textureScale.y);
        }
    }

    if (auto branch = json.find("branch"); branch != json.end() && branch->is_object())
    {
        AssignIfPresent(*branch, "levels", out.branch.levels);
        out.branch.levels = std::min<uint32>(out.branch.levels, 3u);
        AssignMapArray(*branch, "angle", out.branch.angle);
        AssignMapArray(*branch, "children", out.branch.children);
        if (auto force = branch->find("force"); force != branch->end() && force->is_object())
        {
            if (auto direction = force->find("direction"); direction != force->end() && direction->is_object())
                out.branch.forceDirection = ReadVec3(*direction, out.branch.forceDirection);
            AssignIfPresent(*force, "strength", out.branch.forceStrength);
        }
        AssignMapArray(*branch, "gnarliness", out.branch.gnarliness);
        AssignMapArray(*branch, "length", out.branch.length);
        AssignMapArray(*branch, "radius", out.branch.radius);
        AssignMapArray(*branch, "sections", out.branch.sections);
        AssignMapArray(*branch, "segments", out.branch.segments);
        AssignMapArray(*branch, "start", out.branch.start);
        AssignMapArray(*branch, "taper", out.branch.taper);
        AssignMapArray(*branch, "twist", out.branch.twist);
    }

    if (auto leaves = json.find("leaves"); leaves != json.end() && leaves->is_object())
    {
        if (auto it = leaves->find("type"); it != leaves->end() && it->is_string())
            out.leaves.type = LeafTypeFromString(it->get<std::string>(), out.leaves.type);
        if (auto it = leaves->find("billboard"); it != leaves->end() && it->is_string())
            out.leaves.billboard = BillboardModeFromString(it->get<std::string>(), out.leaves.billboard);
        AssignIfPresent(*leaves, "angle", out.leaves.angle);
        AssignIfPresent(*leaves, "count", out.leaves.count);
        AssignIfPresent(*leaves, "start", out.leaves.start);
        AssignIfPresent(*leaves, "size", out.leaves.size);
        AssignIfPresent(*leaves, "sizeVariance", out.leaves.sizeVariance);
        AssignColorIfPresent(*leaves, "tint", out.leaves.tint);
        AssignIfPresent(*leaves, "alphaTest", out.leaves.alphaTest);
        AssignIfPresent(*leaves, "roundedNormals", out.leaves.roundedNormals);
    }

    if (auto trellis = json.find("trellis"); trellis != json.end() && trellis->is_object())
    {
        AssignIfPresent(*trellis, "enabled", out.trellis.enabled);
        if (auto position = trellis->find("position"); position != trellis->end() && position->is_object())
            out.trellis.position = ReadVec3(*position, out.trellis.position);
        AssignIfPresent(*trellis, "width", out.trellis.width);
        AssignIfPresent(*trellis, "height", out.trellis.height);
        AssignIfPresent(*trellis, "spacing", out.trellis.spacing);
        if (auto force = trellis->find("force"); force != trellis->end() && force->is_object())
        {
            AssignIfPresent(*force, "strength", out.trellis.forceStrength);
            AssignIfPresent(*force, "maxDistance", out.trellis.forceMaxDistance);
            AssignIfPresent(*force, "falloff", out.trellis.forceFalloff);
        }
        AssignIfPresent(*trellis, "cylinderRadius", out.trellis.cylinderRadius);
        AssignIfPresent(*trellis, "visible", out.trellis.visible);
        AssignColorIfPresent(*trellis, "color", out.trellis.color);
    }
}

} // namespace

std::vector<PresetInfo> ListPresets()
{
    return {kPresets.begin(), kPresets.end()};
}

bool LoadPreset(std::string_view name, TreeOptions& outOptions, std::string* outError)
{
    if (!IsKnownPreset(name))
    {
        if (outError)
            *outError = "Unknown EZ-Tree preset";
        return false;
    }

    // Presets are staged into the consumer's runtime Assets by the build
    // (StageEditorAssets / the EZTree test staging step). Resolve them relative
    // to the executable, never to the source or vcpkg tree — a shipped or
    // relocated build has neither.
    std::filesystem::path presetsDir =
        PathUtils::GetExecutableDirectory() / "Assets" / "EZTree" / "Presets";
#if defined(__APPLE__)
    {
        // macOS app bundle: staged assets live under Contents/Resources/Assets,
        // a sibling of the Contents/MacOS executable directory.
        std::error_code ec;
        if (!std::filesystem::is_directory(presetsDir, ec))
            presetsDir = PathUtils::GetExecutableDirectory() / ".." / "Resources" / "Assets" /
                         "EZTree" / "Presets";
    }
#endif
    const std::filesystem::path path = presetsDir / (std::string(name) + ".json");

    std::ifstream file(path);
    if (!file)
    {
        if (outError)
            *outError = "Unable to open EZ-Tree preset: " + path.string();
        return false;
    }

    try
    {
        nlohmann::json json = nlohmann::json::parse(file);
        outOptions = MakeDefaultOptions();
        ApplyPresetJson(json, outOptions);
        return true;
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = e.what();
        return false;
    }
}

} // namespace GameEngine::EZTree

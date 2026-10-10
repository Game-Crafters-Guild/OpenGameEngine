#include "Assets/AnimationClip.h"

#include "AnimationClipSchema.h"
#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

// Optional importers
#if defined(GE_HAVE_UFBX)
  #include <ufbx.h>
  #include "Assets/FbxLoaderOptions.h"
  #include "FbxAxisConversion.h"
#endif
#if defined(GE_HAVE_CGLTF)
  #include <cgltf.h>
  #include "Assets/FbxLoaderOptions.h"
  #include "ModelAxisConversion.h"
  #include "GltfAllocationBudget.h"
  #include "GltfAxisConversion.h"
  #include "ModelAssetLoadGltf.h"
#endif

namespace GameEngine
{

namespace
{
#if defined(GE_HAVE_UFBX)
namespace UfbxAnim
{
// Sample rate floor. ufbx exposes the file's authored framerate; we honor
// it but cap the floor at 30 Hz so very low-rate authored clips
// (e.g. 12fps) don't lose curve detail when baked to linear keys.
constexpr double kMinSampleHz = 30.0;

void NormalizeQuaternionInPlace(float r[4])
{
    const float lenSq = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3];
    if (lenSq > 0.0f)
    {
        const float inv = 1.0f / std::sqrt(lenSq);
        r[0] *= inv;
        r[1] *= inv;
        r[2] *= inv;
        r[3] *= inv;
    }
    else
    {
        r[0] = 0.0f;
        r[1] = 0.0f;
        r[2] = 0.0f;
        r[3] = 1.0f;
    }
}

void MakeQuaternionSignContinuous(const AnimKeyframe& prev, AnimKeyframe& current)
{
    const float dot = prev.rotation[0] * current.rotation[0]
                    + prev.rotation[1] * current.rotation[1]
                    + prev.rotation[2] * current.rotation[2]
                    + prev.rotation[3] * current.rotation[3];
    if (dot < 0.0f)
    {
        current.rotation[0] = -current.rotation[0];
        current.rotation[1] = -current.rotation[1];
        current.rotation[2] = -current.rotation[2];
        current.rotation[3] = -current.rotation[3];
    }
}
} // namespace UfbxAnim
#endif
using json = nlohmann::json;

constexpr float kKeyTimeEpsilon = 1e-5f;
constexpr float kDefaultTangentWeight = 1.0f / 3.0f;

std::string NormalizeExtension(std::string extension)
{
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return extension;
}

const char* ToString(AnimPath path)
{
    switch (path)
    {
    case AnimPath::Translation:
        return "Translation";
    case AnimPath::Rotation:
        return "Rotation";
    case AnimPath::Scale:
        return "Scale";
    case AnimPath::MorphWeight:
        return "MorphWeight";
    }

    return "Translation";
}

const char* ToString(AnimTangentType type)
{
    switch (type)
    {
    case AnimTangentType::Auto:    return "Auto";
    case AnimTangentType::Plateau: return "Plateau";
    case AnimTangentType::Clamped: return "Clamped";
    case AnimTangentType::Fixed:   return "Fixed";
    case AnimTangentType::Linear:  return "Linear";
    case AnimTangentType::Flat:    return "Flat";
    case AnimTangentType::Custom:  return "Custom";
    }
    return "Auto";
}

AnimTangentType ParseAnimTangentType(const std::string& value)
{
    if (value == "Plateau") return AnimTangentType::Plateau;
    if (value == "Clamped") return AnimTangentType::Clamped;
    if (value == "Fixed")   return AnimTangentType::Fixed;
    if (value == "Linear")  return AnimTangentType::Linear;
    if (value == "Flat")    return AnimTangentType::Flat;
    if (value == "Custom")  return AnimTangentType::Custom;
    return AnimTangentType::Auto;
}

const char* ToString(AnimExtrapolation mode)
{
    switch (mode)
    {
    case AnimExtrapolation::Constant:        return "Constant";
    case AnimExtrapolation::Linear:          return "Linear";
    case AnimExtrapolation::Cycle:           return "Cycle";
    case AnimExtrapolation::CycleWithOffset: return "CycleWithOffset";
    case AnimExtrapolation::Oscillate:       return "Oscillate";
    }
    return "Constant";
}

AnimExtrapolation ParseAnimExtrapolation(const std::string& value)
{
    if (value == "Linear")          return AnimExtrapolation::Linear;
    if (value == "Cycle")           return AnimExtrapolation::Cycle;
    if (value == "CycleWithOffset") return AnimExtrapolation::CycleWithOffset;
    if (value == "Oscillate")       return AnimExtrapolation::Oscillate;
    return AnimExtrapolation::Constant;
}

const char* ToString(AnimInterp interpolation)
{
    switch (interpolation)
    {
    case AnimInterp::Linear:
        return "Linear";
    case AnimInterp::Step:
        return "Step";
    case AnimInterp::CubicSpline:
        return "CubicSpline";
    }

    return "Linear";
}

AnimPath ParseAnimPath(const std::string& value)
{
    if (value == "Rotation")
        return AnimPath::Rotation;
    if (value == "Scale")
        return AnimPath::Scale;
    if (value == "MorphWeight" || value == "BlendShape" || value == "BlendShapeWeight")
        return AnimPath::MorphWeight;
    return AnimPath::Translation;
}

AnimInterp ParseAnimInterp(const std::string& value)
{
    if (value == "Step")
        return AnimInterp::Step;
    if (value == "CubicSpline")
        return AnimInterp::CubicSpline;
    return AnimInterp::Linear;
}

void InitializeDefaultKeyframe(AnimKeyframe& keyframe)
{
    std::memset(&keyframe, 0, sizeof(keyframe));
    keyframe.rotation[3] = 1.0f;
    keyframe.scale[0] = 1.0f;
    keyframe.scale[1] = 1.0f;
    keyframe.scale[2] = 1.0f;
    std::fill(std::begin(keyframe.inWeight), std::end(keyframe.inWeight), kDefaultTangentWeight);
    std::fill(std::begin(keyframe.outWeight), std::end(keyframe.outWeight), kDefaultTangentWeight);
}

float* GetComponentArray(AnimKeyframe& keyframe, AnimPath path)
{
    switch (path)
    {
    case AnimPath::Translation:
        return keyframe.translation;
    case AnimPath::Rotation:
        return keyframe.rotation;
    case AnimPath::Scale:
        return keyframe.scale;
    case AnimPath::MorphWeight:
        return keyframe.translation;
    }

    return keyframe.translation;
}

const float* GetComponentArray(const AnimKeyframe& keyframe, AnimPath path)
{
    switch (path)
    {
    case AnimPath::Translation:
        return keyframe.translation;
    case AnimPath::Rotation:
        return keyframe.rotation;
    case AnimPath::Scale:
        return keyframe.scale;
    case AnimPath::MorphWeight:
        return keyframe.translation;
    }

    return keyframe.translation;
}

size_t GetComponentCount(AnimPath path)
{
    if (path == AnimPath::MorphWeight)
        return 1u;
    return path == AnimPath::Rotation ? 4u : 3u;
}

float ClampTangentWeight(float weight)
{
    return std::max(0.05f, weight);
}

size_t FindKeyIndex(const std::vector<AnimKeyframe>& keys, float time)
{
    for (size_t index = 0; index < keys.size(); ++index)
    {
        if (std::abs(keys[index].time - time) <= kKeyTimeEpsilon)
            return index;
    }

    return keys.size();
}

AnimKeyframe MakeInterpolatedKeyframe(const AnimChannel& channel, float time)
{
    AnimKeyframe keyframe{};
    InitializeDefaultKeyframe(keyframe);
    keyframe.time = std::max(0.0f, time);

    if (channel.keys.empty())
        return keyframe;

    if (channel.keys.size() == 1 || keyframe.time <= channel.keys.front().time)
    {
        keyframe = channel.keys.front();
        keyframe.time = std::max(0.0f, time);
        return keyframe;
    }

    if (keyframe.time >= channel.keys.back().time)
    {
        keyframe = channel.keys.back();
        keyframe.time = std::max(0.0f, time);
        return keyframe;
    }

    for (size_t index = 0; index + 1 < channel.keys.size(); ++index)
    {
        const AnimKeyframe& first = channel.keys[index];
        const AnimKeyframe& second = channel.keys[index + 1];
        if (keyframe.time < first.time || keyframe.time > second.time)
            continue;

        const float span = std::max(kKeyTimeEpsilon, second.time - first.time);
        const float alpha = std::clamp((keyframe.time - first.time) / span, 0.0f, 1.0f);

        keyframe = first;
        keyframe.time = std::max(0.0f, time);
        float* outValues = GetComponentArray(keyframe, channel.path);
        const float* firstValues = GetComponentArray(first, channel.path);
        const float* secondValues = GetComponentArray(second, channel.path);
        const size_t componentCount = GetComponentCount(channel.path);
        for (size_t componentIndex = 0; componentIndex < componentCount; ++componentIndex)
        {
            outValues[componentIndex] = channel.interp == AnimInterp::Step
                ? firstValues[componentIndex]
                : (firstValues[componentIndex] + (secondValues[componentIndex] - firstValues[componentIndex]) * alpha);
        }
        return keyframe;
    }

    keyframe = channel.keys.back();
    keyframe.time = std::max(0.0f, time);
    return keyframe;
}

json SerializeKeyframe(const AnimKeyframe& keyframe)
{
    json j{
        {"time", keyframe.time},
        {"translation", {keyframe.translation[0], keyframe.translation[1], keyframe.translation[2]}},
        {"rotation", {keyframe.rotation[0], keyframe.rotation[1], keyframe.rotation[2], keyframe.rotation[3]}},
        {"scale", {keyframe.scale[0], keyframe.scale[1], keyframe.scale[2]}},
        {"inTangent", {keyframe.inTangent[0], keyframe.inTangent[1], keyframe.inTangent[2], keyframe.inTangent[3]}},
        {"outTangent", {keyframe.outTangent[0], keyframe.outTangent[1], keyframe.outTangent[2], keyframe.outTangent[3]}},
        {"inWeight", {keyframe.inWeight[0], keyframe.inWeight[1], keyframe.inWeight[2], keyframe.inWeight[3]}},
        {"outWeight", {keyframe.outWeight[0], keyframe.outWeight[1], keyframe.outWeight[2], keyframe.outWeight[3]}}
    };
    // Only serialize tangent types when any component is non-Auto to keep files compact.
    const bool hasCustomTypes =
        keyframe.inTangentType[0] != AnimTangentType::Auto || keyframe.inTangentType[1] != AnimTangentType::Auto ||
        keyframe.inTangentType[2] != AnimTangentType::Auto || keyframe.inTangentType[3] != AnimTangentType::Auto ||
        keyframe.outTangentType[0] != AnimTangentType::Auto || keyframe.outTangentType[1] != AnimTangentType::Auto ||
        keyframe.outTangentType[2] != AnimTangentType::Auto || keyframe.outTangentType[3] != AnimTangentType::Auto;
    if (hasCustomTypes)
    {
        j["inTangentType"]  = {ToString(keyframe.inTangentType[0]),  ToString(keyframe.inTangentType[1]),
                                ToString(keyframe.inTangentType[2]),  ToString(keyframe.inTangentType[3])};
        j["outTangentType"] = {ToString(keyframe.outTangentType[0]), ToString(keyframe.outTangentType[1]),
                                ToString(keyframe.outTangentType[2]), ToString(keyframe.outTangentType[3])};
    }
    if (keyframe.tangentBroken != 0)
        j["tangentBroken"] = keyframe.tangentBroken;
    if (keyframe.hasSegmentInterpOverride)
        j["segmentInterp"] = ToString(keyframe.segmentInterp);
    return j;
}

void DeserializeFloatArray(const json& node, const char* key, float* outValues, size_t valueCount)
{
    if (!node.contains(key) || !node[key].is_array())
        return;

    const json& values = node[key];
    const size_t count = std::min(valueCount, values.size());
    for (size_t index = 0; index < count; ++index)
    {
        outValues[index] = values[index].get<float>();
    }
}

#if defined(GE_HAVE_CGLTF)
// The engine channel a glTF target path animates; empty for a path the glTF
// clip path does not import (weights, or one cgltf does not recognise).
std::optional<AnimPath> EngineChannelPath(cgltf_animation_path_type path)
{
    switch (path)
    {
        case cgltf_animation_path_type_translation: return AnimPath::Translation;
        case cgltf_animation_path_type_rotation: return AnimPath::Rotation;
        case cgltf_animation_path_type_scale: return AnimPath::Scale;
        case cgltf_animation_path_type_weights:
        case cgltf_animation_path_type_invalid:
        case cgltf_animation_path_type_max_enum:
            break;
    }
    return std::nullopt;
}
#endif // GE_HAVE_CGLTF

} // namespace

bool AnimationClip::Load()
{
    if (GetState() == AssetState::Loaded)
        return true;

    // Reuse cached file data when switching animation indices to avoid
    // re-reading the (potentially large) model file from disk.
    if (m_CachedFileData.empty())
    {
        SetState(AssetState::Loading);

        if (!ReadFileBytesShared(GetPath(), m_CachedFileData))
        {
            Logger::Log::Error("AnimationClip: can't read {}", GetPath().string());
            SetState(AssetState::Failed);
            return false;
        }
    }

    return LoadFromData(m_CachedFileData);
}

bool AnimationClip::LoadFromData(const Vector<uint8>& data)
{
    if (GetState() == AssetState::Loaded)
        return true;

    SetState(AssetState::Loading);
    m_Channels.clear();
    m_Duration = 0.0f;
    m_Settings = {};
    m_EventTrack = {};

    const std::string extension = NormalizeExtension(GetExtension());
    bool success = false;

    if (extension == ".anim" || extension == ".animation")
    {
        success = LoadEditableData(data);
    }
    else if (extension == ".gltf" || extension == ".glb")
    {
#if defined(GE_HAVE_CGLTF)
        const GltfDocument gltf = OpenGltfDocument(data, GetPath());
        if (gltf)
        {
            if (gltf->animations_count > 0)
            {
                const ModelImport::AxisConversion axisConversion =
                    ModelImport::MakeEngineConversion(GltfImport::SourceConversion(),
                                                      ResolveFbxLoaderOptions(GetPath()).Axis);
                size_t animationIndex = static_cast<size_t>(m_SelectedAnimation);
                if (animationIndex >= gltf->animations_count)
                    animationIndex = 0;

                const cgltf_animation& animation = gltf->animations[animationIndex];
                m_SelectedAnimation = static_cast<uint32>(animationIndex);
                // A clip whose keys do not fit the file's budget imports no channel, so it does not load.
                GltfAllocationBudget budget(*gltf, GetPath());
                const std::string pastTheBudget = ChargeAnimationKeys(*gltf, animationIndex, budget);
                if (!pastTheBudget.empty())
                    Logger::Log::Error("glTF '{}': the clip of animation {} is not loaded: {}", GetPath().string(),
                                       animationIndex, pastTheBudget);
                const size_t channelCount = pastTheBudget.empty() ? animation.channels_count : 0u;
                for (size_t channelIndex = 0; channelIndex < channelCount; ++channelIndex)
                {
                    const cgltf_animation_channel& sourceChannel = animation.channels[channelIndex];
                    const cgltf_animation_sampler* sampler = sourceChannel.sampler;
                    if (!sampler || !sampler->input || !sampler->output)
                        continue;

                    // cgltf_validate checks the sampler counts only of channels that target
                    // a node, and glTF says to ignore a channel without one.
                    if (!sourceChannel.target_node)
                    {
                        Logger::Log::Warning(
                            "AnimationClip '{}': animation '{}' has a channel with no target node; that channel "
                            "is skipped.",
                            GetName(), animation.name ? animation.name : "<unnamed>");
                        continue;
                    }

                    const std::optional<AnimPath> path = EngineChannelPath(sourceChannel.target_path);
                    if (!path)
                    {
                        Logger::Log::Warning(
                            "AnimationClip '{}': animation '{}' drives the {} path of node '{}', which the engine "
                            "does not animate; that channel is skipped.",
                            GetName(), animation.name ? animation.name : "<unnamed>",
                            sourceChannel.target_path == cgltf_animation_path_type_weights ? "weights" : "unrecognised",
                            sourceChannel.target_node->name ? sourceChannel.target_node->name : "<unnamed>");
                        continue;
                    }

                    const cgltf_accessor* inputAccessor = sampler->input;
                    const cgltf_accessor* outputAccessor = sampler->output;
                    const size_t keyCount = inputAccessor->count;

                    AnimChannel channel{};
                    channel.boneIndex = static_cast<uint32>(sourceChannel.target_node - gltf->nodes);
                    channel.targetName = sourceChannel.target_node->name ? sourceChannel.target_node->name : "";
                    channel.targetNameId = channel.targetName.empty() ? 0u : HashStringId(channel.targetName);
                    channel.path = *path;
                    // Interpolation mode (LINEAR, STEP supported; CUBICSPLINE not yet implemented)
                    if (sampler->interpolation == cgltf_interpolation_type_step)
                        channel.interp = AnimInterp::Step;
                    else if (sampler->interpolation == cgltf_interpolation_type_cubic_spline)
                    {
                        // CubicSpline requires in/out tangent data per keyframe that AnimKeyframe
                        // does not store. Fall back to Linear so the runtime doesn't silently
                        // produce incorrect results.
                        Logger::Log::Warning("AnimationClip: CubicSpline interpolation not supported, falling back to Linear");
                        channel.interp = AnimInterp::Linear;
                    }
                    else
                        channel.interp = AnimInterp::Linear;
                    channel.keys.resize(keyCount);

                    float maxTime = 0.0f;
                    for (size_t keyIndex = 0; keyIndex < keyCount; ++keyIndex)
                    {
                        AnimKeyframe keyframe{};
                        InitializeDefaultKeyframe(keyframe);

                        float time = 0.0f;
                        cgltf_accessor_read_float(inputAccessor, keyIndex, &time, 1);
                        keyframe.time = time;
                        maxTime = std::max(maxTime, time);

                        const size_t outputIndex = channel.interp == AnimInterp::CubicSpline ? (keyIndex * 3u + 1u) : keyIndex;
                        if (channel.interp == AnimInterp::CubicSpline)
                        {
                            cgltf_accessor_read_float(outputAccessor, keyIndex * 3u + 0u, keyframe.inTangent, GetComponentCount(channel.path));
                            cgltf_accessor_read_float(outputAccessor, keyIndex * 3u + 2u, keyframe.outTangent, GetComponentCount(channel.path));
                        }
                        if (sourceChannel.target_path == cgltf_animation_path_type_translation)
                        {
                            cgltf_accessor_read_float(outputAccessor, outputIndex, keyframe.translation, 3);
                            const ModelImport::Vec3 t = ModelImport::ConvertVec3(
                                {keyframe.translation[0], keyframe.translation[1], keyframe.translation[2]},
                                axisConversion, 1.0f);
                            keyframe.translation[0] = t.x;
                            keyframe.translation[1] = t.y;
                            keyframe.translation[2] = t.z;
                            if (channel.interp == AnimInterp::CubicSpline)
                            {
                                const ModelImport::Vec3 inT = ModelImport::ConvertVec3(
                                    {keyframe.inTangent[0], keyframe.inTangent[1], keyframe.inTangent[2]},
                                    axisConversion, 1.0f);
                                const ModelImport::Vec3 outT = ModelImport::ConvertVec3(
                                    {keyframe.outTangent[0], keyframe.outTangent[1], keyframe.outTangent[2]},
                                    axisConversion, 1.0f);
                                keyframe.inTangent[0] = inT.x;
                                keyframe.inTangent[1] = inT.y;
                                keyframe.inTangent[2] = inT.z;
                                keyframe.outTangent[0] = outT.x;
                                keyframe.outTangent[1] = outT.y;
                                keyframe.outTangent[2] = outT.z;
                            }
                        }
                        else if (sourceChannel.target_path == cgltf_animation_path_type_scale)
                        {
                            cgltf_accessor_read_float(outputAccessor, outputIndex, keyframe.scale, 3);
                            const ModelImport::Vec3 s = ModelImport::ConvertScale(
                                {keyframe.scale[0], keyframe.scale[1], keyframe.scale[2]},
                                axisConversion);
                            keyframe.scale[0] = s.x;
                            keyframe.scale[1] = s.y;
                            keyframe.scale[2] = s.z;
                        }
                        else if (sourceChannel.target_path == cgltf_animation_path_type_rotation)
                        {
                            cgltf_accessor_read_float(outputAccessor, outputIndex, keyframe.rotation, 4);
                            const float length = std::sqrt(
                                keyframe.rotation[0] * keyframe.rotation[0] +
                                keyframe.rotation[1] * keyframe.rotation[1] +
                                keyframe.rotation[2] * keyframe.rotation[2] +
                                keyframe.rotation[3] * keyframe.rotation[3]);
                            if (length > 0.0f)
                            {
                                keyframe.rotation[0] /= length;
                                keyframe.rotation[1] /= length;
                                keyframe.rotation[2] /= length;
                                keyframe.rotation[3] /= length;
                            }
                            else
                            {
                                keyframe.rotation[0] = 0.0f;
                                keyframe.rotation[1] = 0.0f;
                                keyframe.rotation[2] = 0.0f;
                                keyframe.rotation[3] = 1.0f;
                            }
                            const ModelImport::Quat q = ModelImport::ConvertQuat(
                                {keyframe.rotation[0], keyframe.rotation[1],
                                 keyframe.rotation[2], keyframe.rotation[3]},
                                axisConversion);
                            keyframe.rotation[0] = q.x;
                            keyframe.rotation[1] = q.y;
                            keyframe.rotation[2] = q.z;
                            keyframe.rotation[3] = q.w;
                        }

                        channel.keys[keyIndex] = keyframe;
                    }

                    std::sort(channel.keys.begin(), channel.keys.end(),
                              [](const AnimKeyframe& left, const AnimKeyframe& right)
                              {
                                  return left.time < right.time;
                              });
                    m_Channels.push_back(std::move(channel));
                    m_Duration = std::max(m_Duration, maxTime);
                }
                success = !m_Channels.empty();
                if (success && animation.extras.data)
                {
                    const std::string refusal =
                        ReadClipSchema(animation.extras.data, m_Channels, m_Duration, m_Settings, m_EventTrack);
                    if (!refusal.empty())
                        Logger::Log::Warning("AnimationClip '{}': the clip schema of animation '{}' is refused: {}. "
                                             "The clip loads without it.",
                                             GetName(), animation.name ? animation.name : "<unnamed>", refusal);
                }
            }
        }
#else
        Logger::Log::Warning("AnimationClip: glTF import requires cgltf");
#endif
    }
    else if (extension == ".fbx")
    {
#if defined(GE_HAVE_UFBX)
        Logger::Log::Info("AnimationClip: loading FBX '{}' ({} bytes)", GetName(), data.size());
        // Load raw — we apply axis + cm→m conversion ourselves on
        // the way out so animation keys land in the same engine target space
        // as the mesh / skeleton produced by ModelAssetLoadFbx.cpp. Letting
        // ufbx convert via target_axes/target_unit_meters splits the
        // transformation across the root node and would mismatch the
        // engine-space bone matrices at sampling time.
        ufbx_load_opts opts{};
        opts.ignore_geometry = true;     // we only need transforms + curves here
        opts.ignore_embedded = true;
        opts.load_external_files = false;
        opts.ignore_missing_external_files = true;

        ufbx_error err{};
        ufbx_scene* scene = ufbx_load_memory(data.data(), data.size(), &opts, &err);
        if (!scene)
        {
            Logger::Log::Warning("AnimationClip: ufbx FBX parse failed: {}",
                                 err.description.length > 0 ? std::string(err.description.data, err.description.length) : "");
        }
        else if (scene->anim_stacks.count == 0)
        {
            Logger::Log::Warning("AnimationClip: FBX contains no animations");
            ufbx_free_scene(scene);
        }
        else
        {
            size_t animationIndex = static_cast<size_t>(m_SelectedAnimation);
            if (animationIndex >= scene->anim_stacks.count)
                animationIndex = 0;
            m_SelectedAnimation = static_cast<uint32>(animationIndex);

            const ufbx_anim_stack* stack = scene->anim_stacks.data[animationIndex];
            const ufbx_anim* anim = stack->anim;

            // Same source FBX axes -> engine axes conversion that
            // ModelAssetLoadFbx applies, so animation keys stay in the same
            // space as the imported skeleton rest pose.
            const float unitScale = scene->settings.unit_meters > 0.0
                ? static_cast<float>(scene->settings.unit_meters)
                : 1.0f;
            const FbxLoaderOptions clipOpts = ResolveFbxLoaderOptions(GetPath());
            ModelImport::AxisConversion axisConversion =
                FbxImport::MakeEngineConversion(scene->settings.axes, clipOpts);

            // Map ufbx node pointer → engine bone index. The skeleton in
            // ModelAssetLoadFbx is built from `scene->nodes[]` in order,
            // so the linear walk here yields identical indices.
            std::unordered_map<const ufbx_node*, uint32> nodeIndexByPtr;
            std::unordered_map<std::string, uint32> nodeIndexByName;
            struct BlendChannelBinding
            {
                uint32 nodeIndex = UINT32_MAX;
                std::string name;
            };
            std::unordered_map<uint32_t, BlendChannelBinding> blendChannelBindingsByElementId;
            nodeIndexByPtr.reserve(scene->nodes.count * 2u);
            nodeIndexByName.reserve(scene->nodes.count * 2u);
            for (size_t ni = 0; ni < scene->nodes.count; ++ni)
            {
                const ufbx_node* node = scene->nodes.data[ni];
                nodeIndexByPtr.emplace(node, static_cast<uint32>(ni));
                if (node->name.length > 0)
                    nodeIndexByName.emplace(std::string(node->name.data, node->name.length),
                                            static_cast<uint32>(ni));
            }
            for (size_t mi = 0; mi < scene->meshes.count; ++mi)
            {
                const ufbx_mesh* mesh = scene->meshes.data[mi];
                if (!mesh || mesh->instances.count == 0)
                    continue;
                const ufbx_node* node = mesh->instances.data[0];
                const uint32 nodeIndex = node ? node->typed_id : UINT32_MAX;
                for (const ufbx_blend_deformer* blend : mesh->blend_deformers)
                {
                    if (!blend)
                        continue;
                    for (const ufbx_blend_channel* channel : blend->channels)
                    {
                        if (!channel)
                            continue;
                        BlendChannelBinding binding{};
                        binding.nodeIndex = nodeIndex;
                        if (channel->name.length > 0)
                            binding.name.assign(channel->name.data, channel->name.length);
                        else if (channel->target_shape && channel->target_shape->name.length > 0)
                            binding.name.assign(channel->target_shape->name.data, channel->target_shape->name.length);
                        else
                            binding.name = "morph_" + std::to_string(blendChannelBindingsByElementId.size());
                        blendChannelBindingsByElementId[channel->element_id] = std::move(binding);
                    }
                }
            }

            // Bake FBX curves/layers through ufbx first. This matches Godot's
            // importer approach and captures non-linear FBX rotation curves,
            // layer weights, and stepped keys more faithfully than direct
            // fixed-rate transform sampling.
            const double fileFps = scene->settings.frames_per_second > 0.0
                ? scene->settings.frames_per_second
                : 30.0;
            const double sampleHz = std::max(fileFps, UfbxAnim::kMinSampleHz);
            ufbx_bake_opts bakeOpts{};
            bakeOpts.trim_start_time = true;
            bakeOpts.resample_rate = sampleHz;
            bakeOpts.minimum_sample_rate = sampleHz;
            bakeOpts.step_handling = UFBX_BAKE_STEP_HANDLING_DEFAULT;
            bakeOpts.key_reduction_enabled = true;
            bakeOpts.key_reduction_rotation = true;

            ufbx_error bakeErr{};
            ufbx_baked_anim* baked = ufbx_bake_anim(scene, anim, &bakeOpts, &bakeErr);
            size_t bakedNodeCount = 0;
            if (!baked)
            {
                Logger::Log::Warning("AnimationClip: ufbx FBX bake failed: {}",
                                     bakeErr.description.length > 0 ? std::string(bakeErr.description.data, bakeErr.description.length) : "");
            }
            else
            {
                bakedNodeCount = baked->nodes.count;
                m_Duration = static_cast<float32>(std::max(0.0, baked->playback_duration));

                for (const ufbx_baked_node& bakedNode : baked->nodes)
                {
                    if (bakedNode.typed_id >= scene->nodes.count)
                        continue;
                    const ufbx_node* node = scene->nodes.data[bakedNode.typed_id];
                    const uint32 boneIndex = bakedNode.typed_id;
                    const std::string targetName = node && node->name.length > 0
                        ? std::string(node->name.data, node->name.length)
                        : std::string{};

                    const StringId nameId = targetName.empty() ? 0u : HashStringId(targetName);

                    AnimChannel transChan{}, rotChan{}, scaleChan{};
                    transChan.boneIndex = rotChan.boneIndex = scaleChan.boneIndex = boneIndex;
                    transChan.targetName = rotChan.targetName = scaleChan.targetName = targetName;
                    transChan.targetNameId = rotChan.targetNameId = scaleChan.targetNameId = nameId;
                    transChan.path = AnimPath::Translation;
                    rotChan.path   = AnimPath::Rotation;
                    scaleChan.path = AnimPath::Scale;
                    transChan.interp = rotChan.interp = scaleChan.interp = AnimInterp::Linear;

                    transChan.keys.reserve(bakedNode.translation_keys.count);
                    rotChan.keys.reserve(bakedNode.rotation_keys.count);
                    scaleChan.keys.reserve(bakedNode.scale_keys.count);

                    for (const ufbx_baked_vec3& tk : bakedNode.translation_keys)
                    {
                        AnimKeyframe k{};
                        InitializeDefaultKeyframe(k);
                        k.time = static_cast<float32>(tk.time);
                        const ufbx_vec3 convertedT =
                            FbxImport::ConvertVec3(tk.value, axisConversion, unitScale);
                        k.translation[0] = static_cast<float32>(convertedT.x);
                        k.translation[1] = static_cast<float32>(convertedT.y);
                        k.translation[2] = static_cast<float32>(convertedT.z);
                        transChan.keys.push_back(k);
                    }
                    for (const ufbx_baked_quat& rk : bakedNode.rotation_keys)
                    {
                        AnimKeyframe k{};
                        InitializeDefaultKeyframe(k);
                        k.time = static_cast<float32>(rk.time);
                        const ufbx_quat convertedR =
                            FbxImport::ConvertQuat(rk.value, axisConversion);
                        k.rotation[0] = static_cast<float32>(convertedR.x);
                        k.rotation[1] = static_cast<float32>(convertedR.y);
                        k.rotation[2] = static_cast<float32>(convertedR.z);
                        k.rotation[3] = static_cast<float32>(convertedR.w);
                        UfbxAnim::NormalizeQuaternionInPlace(k.rotation);
                        if (!rotChan.keys.empty())
                            UfbxAnim::MakeQuaternionSignContinuous(rotChan.keys.back(), k);
                        rotChan.keys.push_back(k);
                    }
                    for (const ufbx_baked_vec3& sk : bakedNode.scale_keys)
                    {
                        AnimKeyframe k{};
                        InitializeDefaultKeyframe(k);
                        k.time = static_cast<float32>(sk.time);
                        const ufbx_vec3 convertedS =
                            FbxImport::ConvertScale(sk.value, axisConversion);
                        k.scale[0] = static_cast<float32>(convertedS.x);
                        k.scale[1] = static_cast<float32>(convertedS.y);
                        k.scale[2] = static_cast<float32>(convertedS.z);
                        scaleChan.keys.push_back(k);
                    }

                    if (!transChan.keys.empty()) m_Channels.push_back(std::move(transChan));
                    if (!rotChan.keys.empty())   m_Channels.push_back(std::move(rotChan));
                    if (!scaleChan.keys.empty()) m_Channels.push_back(std::move(scaleChan));
                }
                for (const ufbx_baked_element& bakedElement : baked->elements)
                {
                    if (bakedElement.element_id >= scene->elements.count)
                        continue;
                    const ufbx_element* element = scene->elements.data[bakedElement.element_id];
                    if (!element || element->type != UFBX_ELEMENT_BLEND_CHANNEL)
                        continue;

                    const auto bindingIt = blendChannelBindingsByElementId.find(element->element_id);
                    if (bindingIt == blendChannelBindingsByElementId.end())
                        continue;

                    for (const ufbx_baked_prop& bakedProp : bakedElement.props)
                    {
                        const std::string propName = bakedProp.name.length > 0
                            ? std::string(bakedProp.name.data, bakedProp.name.length)
                            : std::string{};
                        if (propName != UFBX_DeformPercent || bakedProp.keys.count == 0)
                            continue;

                        AnimChannel morphChan{};
                        morphChan.boneIndex = bindingIt->second.nodeIndex;
                        morphChan.targetName = bindingIt->second.name;
                        morphChan.targetNameId = morphChan.targetName.empty() ? 0u : HashStringId(morphChan.targetName);
                        morphChan.path = AnimPath::MorphWeight;
                        morphChan.interp = AnimInterp::Linear;
                        morphChan.keys.reserve(bakedProp.keys.count);
                        for (const ufbx_baked_vec3& key : bakedProp.keys)
                        {
                            AnimKeyframe outKey{};
                            InitializeDefaultKeyframe(outKey);
                            outKey.time = static_cast<float32>(key.time);
                            outKey.translation[0] = static_cast<float32>(key.value.x / 100.0);
                            morphChan.keys.push_back(outKey);
                        }
                        if (!morphChan.keys.empty())
                            m_Channels.push_back(std::move(morphChan));
                    }
                }
                ufbx_free_baked_anim(baked);
            }

            // Diagnostic: log the maximum rotation amplitude per bone (max angle
            // between any two keyframes). Tells us whether the import captured
            // meaningful animation or whether channels are stuck near rest.
            // Also scan consecutive-key dot products: if any pair has dot<0,
            // the storage is sign-discontinuous and slerp will pick the wrong
            // arc, collapsing apparent motion at sample time.
            uint32 totalNegPairs = 0;
            uint32 channelsWithNegPairs = 0;
            for (const AnimChannel& ch : m_Channels)
            {
                if (ch.path != AnimPath::Rotation || ch.keys.size() < 2) continue;
                float maxAngle = 0.0f;
                const auto& k0 = ch.keys.front();
                const float w0_ = k0.rotation[3];
                const float x0_ = k0.rotation[0];
                const float y0_ = k0.rotation[1];
                const float z0_ = k0.rotation[2];
                float minConsecDot = 1.0f;
                uint32 negPairs = 0;
                for (size_t i = 0; i < ch.keys.size(); ++i)
                {
                    const auto& k = ch.keys[i];
                    // q = inv(k0) * k. Quaternion conjugate inv: (-x,-y,-z,w)
                    const float w_ = w0_*k.rotation[3] + x0_*k.rotation[0] + y0_*k.rotation[1] + z0_*k.rotation[2];
                    const float w_clamped = std::min(std::max(std::abs(w_), 0.0f), 1.0f);
                    const float ang = 2.0f * std::acos(w_clamped) * 57.29577951f;
                    if (ang > maxAngle) maxAngle = ang;

                    if (i > 0)
                    {
                        const auto& kp = ch.keys[i - 1];
                        const float d = kp.rotation[0]*k.rotation[0] + kp.rotation[1]*k.rotation[1]
                                      + kp.rotation[2]*k.rotation[2] + kp.rotation[3]*k.rotation[3];
                        if (d < minConsecDot) minConsecDot = d;
                        if (d < 0.0f) ++negPairs;
                    }
                }
                if (maxAngle > 1.0f)
                {
                    Logger::Log::Info("  ImportRange '{}' rot maxAngle={:.1f}deg over {} keys | "
                                      "minConsecDot={:.4f} negPairs={}{}",
                                      ch.targetName, maxAngle, ch.keys.size(),
                                      minConsecDot, negPairs,
                                      negPairs > 0 ? "  <<< SIGN DISCONTINUITY >>>" : "");
                }
                if (negPairs > 0)
                {
                    totalNegPairs += negPairs;
                    ++channelsWithNegPairs;
                }
            }
            if (totalNegPairs > 0)
            {
                Logger::Log::Warning("AnimationClip: '{}' has {} sign-discontinuous key pairs across "
                                     "{} rotation channels — slerp will collapse motion. Apply sign-"
                                     "continuity fix at import.",
                                     GetName(), totalNegPairs, channelsWithNegPairs);
            }
            success = !m_Channels.empty();
            Logger::Log::Info("AnimationClip: FBX '{}' → {} stack(s), selected={}, duration={:.3f}s, animatedNodes={}, channels={}, success={}",
                              GetName(),
                              scene->anim_stacks.count,
                              animationIndex,
                              m_Duration,
                              bakedNodeCount,
                              m_Channels.size(),
                              success);
            ufbx_free_scene(scene);
        }
#else
        Logger::Log::Warning("AnimationClip: FBX import requires ufbx (GE_HAVE_UFBX)");
#endif
    }
    else if (extension == ".blend")
    {
        // .blend AnimationClips are populated by ModelAssetLoadBlend.cpp's
        // FCurve walker (Phase B3) and registered into the runtime ClipStore
        // by ModelAsset::LoadFromBlendData. Direct LoadFromData on a .blend
        // here is not supported — callers should resolve the embedded clip
        // GUID via ModelAsset::GetEmbeddedClipGuids() and look up the
        // pre-registered clip in ClipStore. Return false so probe-style
        // callers (e.g. thumbnail handler iterating clip indices) cleanly
        // exit without producing a confusing warning. The ModelAsset path
        // remains the authoritative .blend clip extraction route.
        success = false;
    }
    else
    {
        Logger::Log::Warning("AnimationClip: unsupported extension {}", extension);
    }

    SetState(success ? AssetState::Loaded : AssetState::Failed);
    return success;
}

bool AnimationClip::LoadEditableData(const Vector<uint8>& data)
{
    try
    {
        const std::string text(reinterpret_cast<const char*>(data.data()), data.size());
        const json document = json::parse(text);
        m_Channels.clear();
        m_Settings = {};
        m_EventTrack = {};
        m_Duration = document.value("duration", 0.0f);
        m_SelectedAnimation = document.value("selectedAnimationIndex", 0u);

        if (document.contains("source") && document["source"].is_object())
        {
            const json& source = document["source"];
            m_SourcePath = source.value("path", std::string{});
            m_SourceAnimationIndex = source.value("animationIndex", 0u);
        }
        else
        {
            m_SourcePath.clear();
            m_SourceAnimationIndex = 0u;
        }

        if (document.contains("channels") && document["channels"].is_array())
        {
            for (const json& channelJson : document["channels"])
            {
                AnimChannel channel{};
                channel.boneIndex = channelJson.value("boneIndex", 0u);
                channel.targetName = channelJson.value("targetName", String{});
                channel.targetNameId = channel.targetName.empty() ? 0u : HashStringId(channel.targetName);
                channel.path = ParseAnimPath(channelJson.value("path", std::string{"Translation"}));
                channel.interp = ParseAnimInterp(channelJson.value("interpolation", std::string{"Linear"}));
                channel.preInfinity  = ParseAnimExtrapolation(channelJson.value("preInfinity",  std::string{"Constant"}));
                channel.postInfinity = ParseAnimExtrapolation(channelJson.value("postInfinity", std::string{"Constant"}));

                if (channelJson.contains("keys") && channelJson["keys"].is_array())
                {
                    for (const json& keyJson : channelJson["keys"])
                    {
                        AnimKeyframe keyframe{};
                        InitializeDefaultKeyframe(keyframe);
                        keyframe.time = keyJson.value("time", 0.0f);
                        DeserializeFloatArray(keyJson, "translation", keyframe.translation, 3u);
                        DeserializeFloatArray(keyJson, "rotation", keyframe.rotation, 4u);
                        DeserializeFloatArray(keyJson, "scale", keyframe.scale, 3u);
                        DeserializeFloatArray(keyJson, "inTangent", keyframe.inTangent, 4u);
                        DeserializeFloatArray(keyJson, "outTangent", keyframe.outTangent, 4u);
                        DeserializeFloatArray(keyJson, "inWeight", keyframe.inWeight, 4u);
                        DeserializeFloatArray(keyJson, "outWeight", keyframe.outWeight, 4u);
                        for (uint32 componentIndex = 0; componentIndex < 4u; ++componentIndex)
                        {
                            keyframe.inWeight[componentIndex] = ClampTangentWeight(keyframe.inWeight[componentIndex]);
                            keyframe.outWeight[componentIndex] = ClampTangentWeight(keyframe.outWeight[componentIndex]);
                        }
                        if (keyJson.contains("inTangentType") && keyJson["inTangentType"].is_array())
                        {
                            const auto& arr = keyJson["inTangentType"];
                            for (size_t ci = 0; ci < 4u && ci < arr.size(); ++ci)
                                keyframe.inTangentType[ci] = ParseAnimTangentType(arr[ci].get<std::string>());
                        }
                        if (keyJson.contains("outTangentType") && keyJson["outTangentType"].is_array())
                        {
                            const auto& arr = keyJson["outTangentType"];
                            for (size_t ci = 0; ci < 4u && ci < arr.size(); ++ci)
                                keyframe.outTangentType[ci] = ParseAnimTangentType(arr[ci].get<std::string>());
                        }
                        if (keyJson.contains("tangentBroken") && keyJson["tangentBroken"].is_number_unsigned())
                            keyframe.tangentBroken = keyJson["tangentBroken"].get<uint8>();
                        if (keyJson.contains("segmentInterp") && keyJson["segmentInterp"].is_string())
                        {
                            keyframe.segmentInterp = ParseAnimInterp(keyJson["segmentInterp"].get<std::string>());
                            keyframe.hasSegmentInterpOverride = true;
                        }
                        channel.keys.push_back(keyframe);
                    }
                }

                std::sort(channel.keys.begin(), channel.keys.end(),
                          [](const AnimKeyframe& left, const AnimKeyframe& right)
                          {
                              return left.time < right.time;
                          });
                m_Channels.push_back(std::move(channel));
            }
        }

        RecomputeDuration();
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AnimationClip: failed to parse .anim '{}': {}", GetPath().string(), e.what());
        return false;
    }
}

bool AnimationClip::IsEditable() const
{
    const std::string extension = NormalizeExtension(GetExtension());
    return extension == ".anim" || extension == ".animation" || GetPath().empty();
}

void AnimationClip::SetSourceInfo(const std::filesystem::path& sourcePath, uint32 sourceAnimationIndex)
{
    m_SourcePath = sourcePath;
    m_SourceAnimationIndex = sourceAnimationIndex;
}

void AnimationClip::CopyFrom(const AnimationClip& other)
{
    m_SelectedAnimation = other.m_SelectedAnimation;
    m_SourcePath = other.m_SourcePath;
    m_SourceAnimationIndex = other.m_SourceAnimationIndex;
    m_Duration = other.m_Duration;
    m_Channels = other.m_Channels;
    m_Settings = {};
    m_EventTrack = {};
    SetState(other.GetState());
}

void AnimationClip::SetChannelsAndDurationForTest(const std::vector<AnimChannel>& channels, float32 duration)
{
    m_Channels = channels;
    m_Duration = duration;
    SetState(AssetState::Loaded);
}

bool AnimationClip::SetKeyframeTime(size_t channelIndex, float currentTime, float newTime)
{
    if (channelIndex >= m_Channels.size())
        return false;

    std::vector<AnimKeyframe>& keys = m_Channels[channelIndex].keys;
    const size_t foundIndex = FindKeyIndex(keys, currentTime);
    if (foundIndex >= keys.size())
        return false;

    keys[foundIndex].time = std::max(0.0f, newTime);
    std::sort(keys.begin(), keys.end(),
              [](const AnimKeyframe& left, const AnimKeyframe& right)
              {
                  return left.time < right.time;
              });
    RecomputeDuration();
    return true;
}

bool AnimationClip::SetKeyframeComponentValue(size_t channelIndex, float currentTime, uint32 componentIndex, float value)
{
    if (channelIndex >= m_Channels.size())
        return false;

    AnimChannel& channel = m_Channels[channelIndex];
    if (componentIndex >= GetComponentCount(channel.path))
        return false;

    const size_t keyIndex = FindKeyIndex(channel.keys, currentTime);
    if (keyIndex >= channel.keys.size())
        return false;

    float* components = GetComponentArray(channel.keys[keyIndex], channel.path);
    components[componentIndex] = value;

    if (channel.path == AnimPath::Rotation)
    {
        const float length = std::sqrt(
            channel.keys[keyIndex].rotation[0] * channel.keys[keyIndex].rotation[0] +
            channel.keys[keyIndex].rotation[1] * channel.keys[keyIndex].rotation[1] +
            channel.keys[keyIndex].rotation[2] * channel.keys[keyIndex].rotation[2] +
            channel.keys[keyIndex].rotation[3] * channel.keys[keyIndex].rotation[3]);
        if (length > 0.0f)
        {
            channel.keys[keyIndex].rotation[0] /= length;
            channel.keys[keyIndex].rotation[1] /= length;
            channel.keys[keyIndex].rotation[2] /= length;
            channel.keys[keyIndex].rotation[3] /= length;
        }
    }

    return true;
}

bool AnimationClip::SetKeyframeTangent(size_t channelIndex, float currentTime, uint32 componentIndex, bool incoming, float tangent, float weight)
{
    if (channelIndex >= m_Channels.size())
        return false;

    AnimChannel& channel = m_Channels[channelIndex];
    if (componentIndex >= GetComponentCount(channel.path))
        return false;

    const size_t keyIndex = FindKeyIndex(channel.keys, currentTime);
    if (keyIndex >= channel.keys.size())
        return false;

    if (incoming)
    {
        channel.keys[keyIndex].inTangent[componentIndex] = tangent;
        channel.keys[keyIndex].inWeight[componentIndex] = ClampTangentWeight(weight);
    }
    else
    {
        channel.keys[keyIndex].outTangent[componentIndex] = tangent;
        channel.keys[keyIndex].outWeight[componentIndex] = ClampTangentWeight(weight);
    }

    return true;
}

bool AnimationClip::AddKeyframe(size_t channelIndex, float time)
{
    if (channelIndex >= m_Channels.size())
        return false;

    AnimChannel& channel = m_Channels[channelIndex];
    if (FindKeyIndex(channel.keys, time) < channel.keys.size())
        return false;

    channel.keys.push_back(MakeInterpolatedKeyframe(channel, time));
    std::sort(channel.keys.begin(), channel.keys.end(),
              [](const AnimKeyframe& left, const AnimKeyframe& right)
              {
                  return left.time < right.time;
              });

    const size_t newKeyIndex = FindKeyIndex(channel.keys, time);
    if (newKeyIndex < channel.keys.size())
    {
        const size_t componentCount = GetComponentCount(channel.path);
        for (size_t componentIndex = 0; componentIndex < componentCount; ++componentIndex)
        {
            float tangent = 0.0f;
            if (newKeyIndex > 0 && newKeyIndex + 1 < channel.keys.size())
            {
                const AnimKeyframe& prev = channel.keys[newKeyIndex - 1u];
                const AnimKeyframe& next = channel.keys[newKeyIndex + 1u];
                const float* prevValues = GetComponentArray(prev, channel.path);
                const float* nextValues = GetComponentArray(next, channel.path);
                const float span = std::max(kKeyTimeEpsilon, next.time - prev.time);
                tangent = (nextValues[componentIndex] - prevValues[componentIndex]) / span;
            }
            else if (newKeyIndex > 0)
            {
                const AnimKeyframe& prev = channel.keys[newKeyIndex - 1u];
                const float* prevValues = GetComponentArray(prev, channel.path);
                const float* currentValues = GetComponentArray(channel.keys[newKeyIndex], channel.path);
                const float span = std::max(kKeyTimeEpsilon, channel.keys[newKeyIndex].time - prev.time);
                tangent = (currentValues[componentIndex] - prevValues[componentIndex]) / span;
            }
            else if (newKeyIndex + 1 < channel.keys.size())
            {
                const AnimKeyframe& next = channel.keys[newKeyIndex + 1u];
                const float* nextValues = GetComponentArray(next, channel.path);
                const float* currentValues = GetComponentArray(channel.keys[newKeyIndex], channel.path);
                const float span = std::max(kKeyTimeEpsilon, next.time - channel.keys[newKeyIndex].time);
                tangent = (nextValues[componentIndex] - currentValues[componentIndex]) / span;
            }

            channel.keys[newKeyIndex].inTangent[componentIndex] = tangent;
            channel.keys[newKeyIndex].outTangent[componentIndex] = tangent;
        }
    }

    RecomputeDuration();
    return true;
}

bool AnimationClip::AddKeyframeWithData(size_t channelIndex, const AnimKeyframe& keyframe)
{
    if (channelIndex >= m_Channels.size())
        return false;

    AnimChannel& channel = m_Channels[channelIndex];
    if (FindKeyIndex(channel.keys, keyframe.time) < channel.keys.size())
        return false;

    channel.keys.push_back(keyframe);
    std::sort(channel.keys.begin(), channel.keys.end(),
              [](const AnimKeyframe& left, const AnimKeyframe& right)
              {
                  return left.time < right.time;
              });
    RecomputeDuration();
    return true;
}

bool AnimationClip::DuplicateKeyframe(size_t channelIndex, float currentTime, float newTime)
{
    if (channelIndex >= m_Channels.size())
        return false;

    AnimChannel& channel = m_Channels[channelIndex];
    const size_t keyIndex = FindKeyIndex(channel.keys, currentTime);
    if (keyIndex >= channel.keys.size() || FindKeyIndex(channel.keys, newTime) < channel.keys.size())
        return false;

    AnimKeyframe copy = channel.keys[keyIndex];
    copy.time = std::max(0.0f, newTime);
    channel.keys.push_back(copy);
    std::sort(channel.keys.begin(), channel.keys.end(),
              [](const AnimKeyframe& left, const AnimKeyframe& right)
              {
                  return left.time < right.time;
              });
    RecomputeDuration();
    return true;
}

bool AnimationClip::RemoveKeyframe(size_t channelIndex, float currentTime)
{
    if (channelIndex >= m_Channels.size())
        return false;

    AnimChannel& channel = m_Channels[channelIndex];
    const size_t keyIndex = FindKeyIndex(channel.keys, currentTime);
    if (keyIndex >= channel.keys.size())
        return false;

    channel.keys.erase(channel.keys.begin() + static_cast<std::ptrdiff_t>(keyIndex));
    RecomputeDuration();
    return true;
}

bool AnimationClip::SetChannelInterpolation(size_t channelIndex, AnimInterp interpolation)
{
    if (channelIndex >= m_Channels.size())
        return false;

    m_Channels[channelIndex].interp = interpolation;
    return true;
}

bool AnimationClip::SetChannelExtrapolation(size_t channelIndex, AnimExtrapolation pre, AnimExtrapolation post)
{
    if (channelIndex >= m_Channels.size())
        return false;

    m_Channels[channelIndex].preInfinity  = pre;
    m_Channels[channelIndex].postInfinity = post;
    return true;
}

bool AnimationClip::SetKeyframeTangentType(size_t channelIndex, float keyTime, uint32 componentIndex, bool incoming, AnimTangentType type)
{
    if (channelIndex >= m_Channels.size())
        return false;

    AnimChannel& channel = m_Channels[channelIndex];
    if (componentIndex >= GetComponentCount(channel.path))
        return false;

    const size_t keyIndex = FindKeyIndex(channel.keys, keyTime);
    if (keyIndex >= channel.keys.size())
        return false;

    if (incoming)
        channel.keys[keyIndex].inTangentType[componentIndex] = type;
    else
        channel.keys[keyIndex].outTangentType[componentIndex] = type;
    return true;
}

bool AnimationClip::SetKeyframeTangentBroken(size_t channelIndex, float keyTime, uint32 componentIndex, bool broken)
{
    if (channelIndex >= m_Channels.size())
        return false;

    AnimChannel& channel = m_Channels[channelIndex];
    if (componentIndex >= GetComponentCount(channel.path))
        return false;

    const size_t keyIndex = FindKeyIndex(channel.keys, keyTime);
    if (keyIndex >= channel.keys.size())
        return false;

    const uint8 bit = static_cast<uint8>(1u << componentIndex);
    if (broken)
        channel.keys[keyIndex].tangentBroken |= bit;
    else
        channel.keys[keyIndex].tangentBroken &= static_cast<uint8>(~bit);
    return true;
}

bool AnimationClip::SetKeyframeSegmentInterp(size_t channelIndex, float keyTime, AnimInterp interp)
{
    if (channelIndex >= m_Channels.size())
        return false;
    AnimChannel& channel = m_Channels[channelIndex];
    const size_t keyIndex = FindKeyIndex(channel.keys, keyTime);
    if (keyIndex >= channel.keys.size())
        return false;
    channel.keys[keyIndex].segmentInterp = interp;
    channel.keys[keyIndex].hasSegmentInterpOverride = true;
    return true;
}

bool AnimationClip::ResampleBakedChannelToCurve(size_t channelIndex)
{
    if (channelIndex >= m_Channels.size())
        return false;

    AnimChannel& channel = m_Channels[channelIndex];
    if (channel.keys.size() < 2)
        return false;

    if (channel.interp == AnimInterp::CubicSpline)
        return false;

    if (channel.path == AnimPath::Rotation)
    {
        if (channel.interp != AnimInterp::Step)
            return false;

        channel.interp = AnimInterp::Linear;
        RecomputeDuration();
        return true;
    }

    if (channel.interp != AnimInterp::Step && channel.interp != AnimInterp::Linear)
        return false;

    channel.interp = AnimInterp::CubicSpline;
    const size_t keyCount = channel.keys.size();
    const size_t componentCount = GetComponentCount(channel.path);

    for (size_t keyIndex = 0; keyIndex < keyCount; ++keyIndex)
    {
        for (size_t componentIndex = 0; componentIndex < componentCount; ++componentIndex)
        {
            float tangent = 0.0f;
            if (keyIndex > 0 && keyIndex + 1 < keyCount)
            {
                const float* prevValues = GetComponentArray(channel.keys[keyIndex - 1u], channel.path);
                const float* nextValues = GetComponentArray(channel.keys[keyIndex + 1u], channel.path);
                const float span = std::max(kKeyTimeEpsilon, channel.keys[keyIndex + 1u].time - channel.keys[keyIndex - 1u].time);
                tangent = (nextValues[componentIndex] - prevValues[componentIndex]) / span;
            }
            else if (keyIndex > 0)
            {
                const float* prevValues = GetComponentArray(channel.keys[keyIndex - 1u], channel.path);
                const float* currentValues = GetComponentArray(channel.keys[keyIndex], channel.path);
                const float span = std::max(kKeyTimeEpsilon, channel.keys[keyIndex].time - channel.keys[keyIndex - 1u].time);
                tangent = (currentValues[componentIndex] - prevValues[componentIndex]) / span;
            }
            else if (keyIndex + 1 < keyCount)
            {
                const float* currentValues = GetComponentArray(channel.keys[keyIndex], channel.path);
                const float* nextValues = GetComponentArray(channel.keys[keyIndex + 1u], channel.path);
                const float span = std::max(kKeyTimeEpsilon, channel.keys[keyIndex + 1u].time - channel.keys[keyIndex].time);
                tangent = (nextValues[componentIndex] - currentValues[componentIndex]) / span;
            }

            channel.keys[keyIndex].inTangent[componentIndex] = tangent;
            channel.keys[keyIndex].outTangent[componentIndex] = tangent;
        }

        for (size_t c = componentCount; c < 4u; ++c)
        {
            channel.keys[keyIndex].inTangent[c] = 0.0f;
            channel.keys[keyIndex].outTangent[c] = 0.0f;
        }
    }

    RecomputeDuration();
    return true;
}

size_t AnimationClip::FindOrAddChannel(uint32 boneIndex, const String& targetName, AnimPath path)
{
    for (size_t index = 0; index < m_Channels.size(); ++index)
    {
        if (m_Channels[index].boneIndex == boneIndex && m_Channels[index].path == path)
        {
            if (m_Channels[index].targetName.empty() && !targetName.empty())
            {
                m_Channels[index].targetName = targetName;
                m_Channels[index].targetNameId = HashStringId(targetName);
            }
            return index;
        }
    }

    AnimChannel channel{};
    channel.boneIndex = boneIndex;
    channel.targetName = targetName;
    channel.targetNameId = targetName.empty() ? 0u : HashStringId(targetName);
    channel.path = path;
    channel.interp = AnimInterp::Linear;
    m_Channels.push_back(std::move(channel));
    return m_Channels.size() - 1u;
}

bool AnimationClip::SaveToPath(const std::filesystem::path& path) const
{
    try
    {
        Vector<uint8> data;
        if (!SaveToData(data))
            return false;

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
            return false;

        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return output.good();
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AnimationClip: failed to save '{}': {}", path.string(), e.what());
        return false;
    }
}

bool AnimationClip::SaveToData(Vector<uint8>& outData) const
{
    try
    {
        json document;
        document["version"] = 1;
        document["duration"] = m_Duration;
        document["selectedAnimationIndex"] = m_SelectedAnimation;
        if (!m_SourcePath.empty())
        {
            document["source"] = {
                {"path", m_SourcePath.string()},
                {"animationIndex", m_SourceAnimationIndex}
            };
        }

        json channels = json::array();
        for (const AnimChannel& channel : m_Channels)
        {
            json keys = json::array();
            for (const AnimKeyframe& keyframe : channel.keys)
                keys.push_back(SerializeKeyframe(keyframe));

            json channelJson{
                {"boneIndex", channel.boneIndex},
                {"targetName", channel.targetName},
                {"path", ToString(channel.path)},
                {"interpolation", ToString(channel.interp)},
                {"keys", std::move(keys)}
            };
            if (channel.preInfinity != AnimExtrapolation::Constant)
                channelJson["preInfinity"] = ToString(channel.preInfinity);
            if (channel.postInfinity != AnimExtrapolation::Constant)
                channelJson["postInfinity"] = ToString(channel.postInfinity);
            channels.push_back(std::move(channelJson));
        }
        document["channels"] = std::move(channels);

        const std::string text = document.dump(2);
        outData.assign(text.begin(), text.end());
        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("AnimationClip: failed to serialize '{}': {}", GetPath().string(), e.what());
        return false;
    }
}

bool AnimationClip::RestoreFromData(const Vector<uint8>& data)
{
    const AssetState previousState = GetState();
    SetState(AssetState::Loading);
    if (!LoadEditableData(data))
    {
        SetState(previousState);
        return false;
    }

    SetState(AssetState::Loaded);
    return true;
}

void AnimationClip::RecomputeDuration()
{
    float maxTime = 0.0f;
    for (const AnimChannel& channel : m_Channels)
    {
        for (const AnimKeyframe& keyframe : channel.keys)
        {
            maxTime = std::max(maxTime, keyframe.time);
        }
    }
    m_Duration = maxTime;
}

void AnimationClip::Unload()
{
    m_Channels.clear();
    m_Duration = 0.0f;
    m_Settings = {};
    m_EventTrack = {};
    SetState(AssetState::Unloaded);
}

std::vector<std::string> AnimationClip::EnumerateAnimationNames() const
{
    if (GetPath().empty())
        return {};

    const std::string extension = NormalizeExtension(GetPath().extension().string());
    if (extension == ".anim" || extension == ".animation")
    {
        const std::string stem = GetPath().stem().string();
        return {stem.empty() ? "Animation" : stem};
    }

#if defined(GE_HAVE_CGLTF)
    if ((extension == ".gltf" || extension == ".glb") && !m_CachedFileData.empty())
    {
        cgltf_options options{};
        cgltf_data* gltf = nullptr;
        if (cgltf_parse(&options, m_CachedFileData.data(), m_CachedFileData.size(), &gltf) == cgltf_result_success && gltf)
        {
            std::vector<std::string> names;
            names.reserve(gltf->animations_count);
            for (size_t i = 0; i < gltf->animations_count; ++i)
            {
                const char* name = gltf->animations[i].name;
                if (name && name[0] != '\0')
                    names.emplace_back(name);
                else
                    names.push_back("Animation " + std::to_string(i));
            }
            cgltf_free(gltf);
            return names;
        }
        if (gltf) cgltf_free(gltf);
    }
#endif

    return {};
}

} // namespace GameEngine

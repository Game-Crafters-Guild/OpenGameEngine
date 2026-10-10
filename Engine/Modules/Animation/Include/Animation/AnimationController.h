#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Animation
{

enum class AnimationParameterType : uint8
{
    Float,
    Int,
    Bool,
    Trigger
};

enum class AnimationTransitionComparison : uint8
{
    Always,
    Equals,
    NotEquals,
    Greater,
    Less
};

enum class BlendTreeType : uint8
{
    None,
    Blend1D,
    Blend2D
};

struct AnimationParameter
{
    std::string Name;
    AnimationParameterType Type = AnimationParameterType::Float;
    float32 DefaultFloat = 0.0f;
    int32 DefaultInt = 0;
    bool DefaultBool = false;
};

struct AnimationTransitionConditionDef
{
    std::string Parameter;
    AnimationTransitionComparison Comparison = AnimationTransitionComparison::Always;
    float32 FloatValue = 0.0f;
    int32 IntValue = 0;
    bool BoolValue = false;
};

struct AnimationStateMotion
{
    GUID AssetGuid;
    AssetType Type = AssetType::Animation;
    std::string LibraryName;
    float32 Speed = 1.0f;
    bool Loop = true;
};

struct AnimationBlendTreeChild
{
    AnimationStateMotion Motion;
    float32 ThresholdX = 0.0f;
    float32 ThresholdY = 0.0f;
};

struct AnimationBlendTree
{
    BlendTreeType Type = BlendTreeType::None;
    std::string ParameterX;
    std::string ParameterY;
    std::vector<AnimationBlendTreeChild> Children;
};

struct AnimationControllerState
{
    std::string Name;
    AnimationStateMotion Motion;
    AnimationBlendTree BlendTree;
};

struct AnimationControllerTransition
{
    std::string FromState;
    std::string ToState;
    float32 DurationSeconds = 0.2f;
    float32 ExitTimeSeconds = -1.0f;
    bool CanInterrupt = true;
    std::vector<AnimationTransitionConditionDef> Conditions;
};

class AnimationController : public ::GameEngine::Asset
{
public:
    static constexpr int32 kSchemaVersion = 1;

    AnimationController(const ::GameEngine::GUID& guid, const std::filesystem::path& path)
        : ::GameEngine::Asset(guid, ::GameEngine::AssetType::AnimationController, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    bool SaveToData(Vector<uint8>& outData) const;

    const std::vector<AnimationParameter>& Parameters() const { return m_Parameters; }
    const std::vector<AnimationControllerState>& States() const { return m_States; }
    const std::vector<AnimationControllerTransition>& Transitions() const { return m_Transitions; }
    const std::string& EntryState() const { return m_EntryState; }

    const AnimationControllerState* FindState(const std::string& name) const;
    void SetDataForTest(std::vector<AnimationParameter> parameters,
                        std::vector<AnimationControllerState> states,
                        std::vector<AnimationControllerTransition> transitions,
                        std::string entryState);

private:
    bool ParseJson(const std::string& text);
    std::string SerializeJson() const;

    std::vector<AnimationParameter> m_Parameters;
    std::vector<AnimationControllerState> m_States;
    std::vector<AnimationControllerTransition> m_Transitions;
    std::string m_EntryState;
};

const char* AnimationParameterTypeToString(AnimationParameterType type);
AnimationParameterType AnimationParameterTypeFromString(const std::string& value);
const char* AnimationTransitionComparisonToString(AnimationTransitionComparison comparison);
AnimationTransitionComparison AnimationTransitionComparisonFromString(const std::string& value);
const char* BlendTreeTypeToString(BlendTreeType type);
BlendTreeType BlendTreeTypeFromString(const std::string& value);

} // namespace Animation
} // namespace GameEngine

#pragma once

#include <memory>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine::Animation
{

class AnimGraphNode;
class AnimationMontage;
struct AnimationGraphPlayer;

// Serializes and deserializes animation graphs and montages to/from JSON.
// Each node type is identified by a "type" string in the JSON representation.
// Transition conditions use a declarative format (param, op, value) rather
// than the runtime std::function predicates, so a round-trip through JSON
// produces functionally equivalent graphs.
class AnimationGraphSerializer
{
public:
    static nlohmann::json Serialize(const AnimationGraphPlayer& player);
    static std::unique_ptr<AnimationGraphPlayer> Deserialize(const nlohmann::json& json);

    static nlohmann::json SerializeMontage(const AnimationMontage& montage);
    static std::unique_ptr<AnimationMontage> DeserializeMontage(const nlohmann::json& json);

private:
    static nlohmann::json SerializeNode(const AnimGraphNode* node);
    static std::unique_ptr<AnimGraphNode> DeserializeNode(const nlohmann::json& json);
};

} // namespace GameEngine::Animation

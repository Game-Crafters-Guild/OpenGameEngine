#pragma once

#include "Animation/AnimationGraphPlayer.h"

#include <memory>
#include <nlohmann/json.hpp>

namespace GameEngine { namespace Engine::Renderer {

/** True when `doc` is a Graph::Model object (`nodes` array, no `rootNode`). */
bool LooksLikeAuthoringModel(const nlohmann::json& doc);

/** Compile pose wires from the unique OutputPose sink. Nullptr on failure. */
std::unique_ptr<Animation::AnimationGraphPlayer> CompileAuthoringModel(const nlohmann::json& doc);

} } // namespace GameEngine::Engine::Renderer

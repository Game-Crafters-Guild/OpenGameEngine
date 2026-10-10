#pragma once

#include <functional>
#include <string>

namespace GameEngine
{
class Label;
class Slider;
class Vector3Field;
}
#include "Rendering/Common/Math.h"

namespace Editor
{

void AddLabelDoubleClickReset(GameEngine::Label* label,
                              GameEngine::Slider* slider,
                              float defaultValue,
                              const std::string& prefKey = "",
                              std::function<void(float)> onReset = nullptr);

void AddLabelDoubleClickReset(GameEngine::Label* label,
                              GameEngine::Vector3Field* field,
                              const GameEngine::Rendering::Vector3& defaultValue,
                              std::function<void(const GameEngine::Rendering::Vector3&)> onReset = nullptr);

} // namespace Editor

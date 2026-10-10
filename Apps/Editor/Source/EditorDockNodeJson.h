#pragma once

#include "UI/Layout/Docking.h"

#include <memory>

#include <nlohmann/json.hpp>

namespace GameEngine
{

nlohmann::json DockNodeToJson(const DockNode* n);
std::unique_ptr<DockNode> DockNodeFromJson(const nlohmann::json& j);
std::unique_ptr<DockNode> CloneDockNode(const DockNode* src);

} // namespace GameEngine

#pragma once
#include "Types/Types.h"
#include <type_traits>

namespace GameEngine::Components
{

struct NavigationObstacle
{
    float32 Radius = 0.5f;
    float32 Height = 2.0f;
};

static_assert(std::is_trivially_copyable_v<NavigationObstacle>);
static_assert(std::is_standard_layout_v<NavigationObstacle>);

} // namespace GameEngine::Components

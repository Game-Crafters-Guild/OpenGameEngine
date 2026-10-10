#pragma once
#include "Types/Types.h"
#include <type_traits>

namespace GameEngine::Components
{

struct NavigationDebugSettings
{
    bool ShowGrid = false;
    bool ShowNavMesh = false;
    bool ShowPaths = false;
    bool ShowAgentRadii = false;
    bool ShowCellCosts = false;
    bool ShowReservations = false;
    float32 GridLineThickness = 1.0f;
    float32 PathLineThickness = 2.0f;
};

static_assert(std::is_trivially_copyable_v<NavigationDebugSettings>);
static_assert(std::is_standard_layout_v<NavigationDebugSettings>);

} // namespace GameEngine::Components

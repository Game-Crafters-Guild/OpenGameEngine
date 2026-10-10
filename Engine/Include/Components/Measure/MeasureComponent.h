#pragma once

#include "ECS/ECS.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

// Editor-authored measurement marker, drawn only by the editor's gizmo. A game
// export strips it from the staged scenes and keeps the entity it sits on.
// @ge-editor-only
struct MeasureComponent
{
    float32 Start[3] = {0.0f, 0.0f, 0.0f};
    float32 End[3] = {0.0f, 0.0f, 0.0f};
    ECS::EntityHandle StartEntity{};
    ECS::EntityHandle EndEntity{};
    float32 Color[4] = {1.0f, 0.78f, 0.22f, 1.0f};
    bool Is2D = false;
    uint8 _Pad[3] = {};
};

static_assert(std::is_trivially_copyable_v<MeasureComponent>,
              "MeasureComponent must be trivially copyable for ECS storage");
static_assert(std::is_standard_layout_v<MeasureComponent>,
              "MeasureComponent must be standard layout for ECS storage");

} // namespace GameEngine::Components
